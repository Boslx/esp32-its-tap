use std::{
    io,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use clap::Parser;
use log::{info, warn};
use tappers::{tokio::AsyncTap, Interface};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio_serial::SerialPortBuilderExt;

use etherparse::{EtherType, Ethernet2Header};
use libwifi::{
    frame::{
        components::{DataHeader, FrameControl, MacAddress, SequenceControl},
        Frame, QosData,
    },
    parse_frame, Addresses, FrameProtocolVersion, FrameSubType, FrameType,
};

mod error;
mod serial_proto;

pub mod its_proto {
    include!(concat!(env!("OUT_DIR"), "/its.rs"));
}

/// Expected firmware version string reported by the board.
/// The bridge logs a warning if the board reports a different version.
const EXPECTED_FW_VERSION: &str = "1.1";

use error::{Error, Result};
use its_proto::{to_board::Msg, Frame as ProtoFrame, ToBoard};
use serial_proto::{encode_to_board, FrameDecoder};

// -- CLI --------------------------------------------------------------------

#[derive(Parser, Debug)]
#[command(
    name = "its-g5-tap-bridge",
    about = "Bidirectional serial<->TAP bridge for ITS-G5"
)]
struct Cli {
    /// TAP interface name
    #[arg(long, default_value = "its-g5-tap")]
    tap: String,

    /// Serial port path
    #[arg(long, default_value = "/dev/ttyACM0")]
    serial: String,

    /// Serial baud rate
    #[arg(long, default_value = "115200")]
    baud: u32,
}

// -- 802.11 <-> Ethernet conversion -------------------------------------------

/// Overhead of LLC (3) + SNAP OUI (3) before the EtherType.
const LLC_SNAP_HEADER: usize = 8;

/// 802.11-specific header fields that are not present in an Ethernet frame.
/// These must be preserved for a lossless round-trip conversion.
struct WifiMeta {
    frame_ctrl: [u8; 2],
    duration: [u8; 2],
    addr3: [u8; 6], // BSSID
    seq_ctrl: [u8; 2],
    qos_ctrl: [u8; 2],
}

/// Default 802.11 metadata extracted from `WIFI_FRAME`.
/// Use with `ethernet_to_wifi` for sending frames with the same
/// 802.11 header parameters (BSSID, QoS, etc.) as the captured frame.
const DEFAULT_WIFI_META: WifiMeta = WifiMeta {
    frame_ctrl: [0x88, 0x00],
    duration: [0x00, 0x00],
    addr3: [0xff, 0xff, 0xff, 0xff, 0xff, 0xff],
    seq_ctrl: [0xf0, 0x18],
    qos_ctrl: [0x25, 0x00],
};

/// Convert an IEEE 802.11 QoS Data frame to an Ethernet II frame.
///
/// Returns the Ethernet frame bytes and the 802.11 metadata needed
/// to reconstruct the original 802.11 frame.
fn wifi_to_ethernet(wifi: &[u8]) -> (Vec<u8>, WifiMeta) {
    // Parse via libwifi
    let frame = parse_frame(wifi, false).expect("valid 802.11 frame");
    let qos = match &frame {
        Frame::QosData(q) => q,
        _ => panic!("expected QosData frame"),
    };

    // MAC addresses
    let src_mac = qos.src().expect("source address").0;
    let dst_mac = qos.dest().0;

    // EtherType from SNAP (bytes 6..8 of the data field)
    let ether_type = u16::from_be_bytes([qos.data[6], qos.data[7]]);
    // GeoNetworking payload is after LLC+SNAP
    let geonet_payload = &qos.data[LLC_SNAP_HEADER..];

    // Build Ethernet II header via etherparse
    let eth = Ethernet2Header {
        destination: dst_mac,
        source: src_mac,
        ether_type: EtherType(ether_type),
    };

    let mut frame_buf = Vec::with_capacity(14 + geonet_payload.len());
    frame_buf.extend_from_slice(&eth.to_bytes());
    frame_buf.extend_from_slice(geonet_payload);

    // Preserve 802.11-specific header fields using libwifi types
    let meta = WifiMeta {
        frame_ctrl: qos.header.frame_control.encode(),
        duration: qos.header.duration,
        addr3: qos.header.address_3.0,
        seq_ctrl: qos.header.sequence_control.encode(),
        qos_ctrl: qos.header.qos.expect("QoS Data frame has QoS control"),
    };

    (frame_buf, meta)
}

/// Convert an Ethernet II frame back to an IEEE 802.11 QoS Data frame,
/// using the provided 802.11 metadata to restore header fields that have
/// no Ethernet equivalent.
fn ethernet_to_wifi(eth: &[u8], meta: &WifiMeta) -> Vec<u8> {
    // Parse the Ethernet frame via etherparse
    let eth_hdr = Ethernet2Header::from_slice(eth)
        .expect("valid Ethernet II frame")
        .0;

    // Reconstruct the FrameControl from preserved raw bytes
    let frame_ctrl_raw = meta.frame_ctrl;
    let frame_ctrl = FrameControl {
        protocol_version: FrameProtocolVersion::PV0,
        frame_type: FrameType::Data,
        frame_subtype: FrameSubType::QosData,
        flags: frame_ctrl_raw[1],
    };

    // Construct the DataHeader using libwifi types
    let header = DataHeader {
        frame_control: frame_ctrl,
        duration: meta.duration,
        address_1: MacAddress(eth_hdr.destination),
        address_2: MacAddress(eth_hdr.source),
        address_3: MacAddress(meta.addr3),
        sequence_control: SequenceControl {
            fragment_number: meta.seq_ctrl[0] & 0x0F,
            sequence_number: u16::from_le_bytes(meta.seq_ctrl) >> 4,
        },
        address_4: None,
        qos: Some(meta.qos_ctrl),
    };

    // Build the LLC + SNAP + payload data field
    let mut data =
        Vec::with_capacity(LLC_SNAP_HEADER + eth.len().saturating_sub(Ethernet2Header::LEN));
    data.extend_from_slice(&[0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00]); // LLC + SNAP OUI
    data.extend_from_slice(&eth_hdr.ether_type.0.to_be_bytes()); // EtherType
    data.extend_from_slice(&eth[Ethernet2Header::LEN..]); // payload

    // Construct the full QosData frame
    let qos = QosData {
        header,
        eapol_key: None,
        data,
    };

    // Encode: 802.11 header via DataHeader::encode() + LLC+SNAP+payload already in qos.data
    let mut out = qos.header.encode();
    out.extend_from_slice(&qos.data);
    out
}

/// Read the MAC address of a network interface from sysfs.
fn read_mac(ifname: &str) -> io::Result<[u8; 6]> {
    let path = format!("/sys/class/net/{ifname}/address");
    let s = std::fs::read_to_string(&path)?;
    let s = s.trim();
    let mut mac = [0u8; 6];
    for (i, byte_str) in s.split(':').enumerate() {
        mac[i] = u8::from_str_radix(byte_str, 16)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    }
    Ok(mac)
}

// -- Helpers ----------------------------------------------------------------

/// Pretty-print a MAC address from 6 bytes.
fn fmt_mac(mac: &[u8]) -> String {
    format!(
        "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    )
}

// -- Bridge tasks -----------------------------------------------------------

/// Serial -> TAP: read framed protobuf from the serial port, decode
/// `FromBoard` messages, convert 802.11 -> Ethernet, and inject into TAP.
async fn serial_to_tap(
    mut port: impl AsyncReadExt + Unpin,
    tap: Arc<AsyncTap>,
    last_hb: Arc<Mutex<Option<Instant>>>,
) -> Result<()> {
    info!("Serial: listening");

    let mut decoder = FrameDecoder::new();
    let mut buf = [0u8; 2048];
    let mut mismatch_warned = false;

    loop {
        let n = port.read(&mut buf).await?;
        if n == 0 {
            continue;
        }

        let msgs = decoder.push(&buf[..n])?;
        for msg in msgs {
            match msg.msg {
                Some(its_proto::from_board::Msg::Heartbeat(hb)) => {
                    *last_hb.lock().unwrap() = Some(Instant::now());

                    let version = hb.version.clone();

                    if !mismatch_warned && version != EXPECTED_FW_VERSION {
                        mismatch_warned = true;
                        warn!(
                            "Firmware version mismatch: board reports '{}', bridge expects '{}'",
                            version, EXPECTED_FW_VERSION,
                        );
                    }

                    info!(
                        "HB [#{}] v{}  uptime={}s  heap={}K  min_heap={}K  \
                         TX={}  RX={}  dropped={}  MAC={}",
                        hb.hb_seq,
                        version,
                        hb.uptime_s,
                        hb.free_heap / 1024,
                        hb.min_free_heap / 1024,
                        hb.tx_count,
                        hb.rx_count,
                        hb.rx_dropped,
                        fmt_mac(&hb.src_mac),
                    );
                }
                Some(its_proto::from_board::Msg::ReceivedFrame(rf)) => {
                    let wifi_data = &rf.frame.unwrap_or_default().frame_data;
                    if wifi_data.is_empty() {
                        warn!("RX: empty frame from serial, skipping");
                        continue;
                    }
                    let (eth_frame, _meta) = wifi_to_ethernet(wifi_data);
                    info!(
                        "serial->TAP: {}B 802.11 -> {}B Eth  rssi={}dBm  src={}",
                        wifi_data.len(),
                        eth_frame.len(),
                        rf.rssi,
                        fmt_mac(&rf.src_mac),
                    );
                    tap.send(&eth_frame).await?;
                }
                None => {
                    // Unknown oneof variant -- silently skip
                }
            }
        }
    }
}

/// TAP -> Serial: read Ethernet frames from TAP, convert to 802.11 QoS Data,
/// wrap in `ToBoard` protobuf, frame it, and write to the serial port.
async fn tap_to_serial(mut port: impl AsyncWriteExt + Unpin, tap: Arc<AsyncTap>) -> Result<()> {
    info!("TAP->serial: ready");

    let mut buf = [0u8; 65536];

    loop {
        let n = tap.recv(&mut buf).await?;
        let eth_frame = &buf[..n];

        let wifi_frame = ethernet_to_wifi(eth_frame, &DEFAULT_WIFI_META);

        let to_board = ToBoard {
            msg: Some(Msg::Frame(ProtoFrame {
                frame_data: wifi_frame,
            })),
        };

        let wire = encode_to_board(&to_board)?;
        port.write_all(&wire).await?;

        info!("TAP->serial: {}B Eth -> {}B 802.11", n, wire.len(),);
    }
}

// -- Main -------------------------------------------------------------------

#[tokio::main(flavor = "current_thread")]
async fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("warn")).init();
    let cli = Cli::parse();

    if let Err(e) = run(cli).await {
        eprintln!("Error: {e}");
        log::error!("{e}");
        std::process::exit(1);
    }
}

async fn run(cli: Cli) -> Result<()> {
    // -- Open TAP device --
    let if_name = Interface::new(&cli.tap).map_err(|e| Error::Io {
        context: "Failed to create TAP interface",
        source: e,
    })?;
    let mut tap = AsyncTap::new_named(if_name).map_err(|e| Error::Io {
        context: "Failed to open TAP device",
        source: e,
    })?;
    tap.set_up().map_err(|e| Error::Io {
        context: "Failed to bring TAP interface up",
        source: e,
    })?;
    let tap = Arc::new(tap);

    let name = tap.name().map_err(|e| Error::Io {
        context: "Failed to get TAP device name",
        source: e,
    })?;
    let name_str = name.name().to_string_lossy().into_owned();
    let own_mac = read_mac(&name_str).map_err(|e| Error::Io {
        context: "Failed to read TAP MAC address from sysfs",
        source: e,
    })?;
    info!("TAP device '{name_str}' is up. MAC: {}", fmt_mac(&own_mac));
    info!("Using DEFAULT_WIFI_META for Ethernet->802.11 conversion.");
    info!("Expecting firmware version {}", EXPECTED_FW_VERSION);

    // -- Open serial port once, split into read/write halves --
    let serial_port = tokio_serial::new(&cli.serial, cli.baud)
        .open_native_async()
        .map_err(|source| Error::Serial {
            port: cli.serial.clone(),
            baud: cli.baud,
            source,
        })?;
    let (serial_rx, mut serial_tx) = tokio::io::split(serial_port);
    info!("Serial: {} @ {} baud -- opened", cli.serial, cli.baud);

    // -- Request heartbeat on startup to verify correct firmware --
    info!("Requesting heartbeat from board...");
    let hb_request = ToBoard {
        msg: Some(Msg::RequestHb(true)),
    };
    let hb_wire = encode_to_board(&hb_request)?;
    serial_tx.write_all(&hb_wire).await?;
    info!("Heartbeat request sent, awaiting response...");

    // -- Shared heartbeat state for watchdog --
    let last_hb = Arc::new(Mutex::new(None::<Instant>));
    let last_hb_watchdog = last_hb.clone();

    // -- Heartbeat watchdog: warn if no heartbeat for > 3 intervals --
    let hb_timeout = Duration::from_secs(35); // 3× interval + some slack
    tokio::spawn(async move {
        let mut check = tokio::time::interval(Duration::from_secs(5));
        loop {
            check.tick().await;
            let elapsed = last_hb_watchdog
                .lock()
                .unwrap()
                .map(|t| t.elapsed())
                .unwrap_or(Duration::from_secs(u64::MAX));
            if elapsed > hb_timeout {
                warn!(
                    "No heartbeat for {:.0}s — the ESP32 may be dead or disconnected!",
                    elapsed.as_secs(),
                );
            }
        }
    });

    // -- Launch bridge tasks --
    let tap_rx = tap.clone();

    let s2t = tokio::spawn(serial_to_tap(serial_rx, tap_rx, last_hb));
    let t2s = tokio::spawn(tap_to_serial(serial_tx, tap.clone()));

    tokio::select! {
        res = s2t => { res.unwrap()?; }
        res = t2s => { res.unwrap()?; }
    }

    Ok(())
}

// -- Tests ------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    const WIFI_FRAME: [u8; 448] = [
        0x88, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xae, 0x93, 0x1b, 0xf6, 0x5e,
        0x6b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0, 0x18, 0x25, 0x00, 0xaa, 0xaa, 0x03, 0x00,
        0x00, 0x00, 0x89, 0x47, 0x12, 0x00, 0x05, 0x01, 0x03, 0x81, 0x00, 0x40, 0x03, 0x80, 0x81,
        0xae, 0x20, 0x50, 0x02, 0x80, 0x00, 0x8a, 0x01, 0x00, 0x14, 0x00, 0xae, 0x93, 0x1b, 0xf6,
        0x5e, 0x6b, 0x34, 0x84, 0xd5, 0x2f, 0x1d, 0x1c, 0x8d, 0xf4, 0x05, 0x76, 0x43, 0x18, 0x87,
        0xd6, 0x02, 0xeb, 0x00, 0x00, 0xa0, 0x00, 0x07, 0xd1, 0x00, 0x00, 0x02, 0x02, 0x1b, 0xf6,
        0x5e, 0x6b, 0xd6, 0x53, 0x40, 0x5a, 0x58, 0x2e, 0xf2, 0x2e, 0x18, 0x03, 0x0c, 0x22, 0x34,
        0x22, 0xc8, 0x06, 0x42, 0x6f, 0x90, 0x58, 0x2e, 0xb0, 0xa3, 0xe6, 0xfe, 0x02, 0x96, 0x8a,
        0x7b, 0x37, 0xfe, 0xe9, 0xff, 0xce, 0x10, 0x3f, 0xff, 0x94, 0x19, 0x80, 0x10, 0x55, 0xfe,
        0x6a, 0x7d, 0xdd, 0x59, 0x00, 0x00, 0x13, 0x2f, 0xf0, 0xc3, 0xeb, 0x0e, 0xc6, 0x70, 0x00,
        0xcb, 0x7f, 0x7e, 0xdf, 0x49, 0x46, 0x33, 0x80, 0x06, 0xeb, 0xfc, 0x34, 0xfa, 0x74, 0xb2,
        0x00, 0x00, 0x31, 0x5f, 0xe4, 0x47, 0xd4, 0x91, 0x8c, 0xe0, 0x01, 0x92, 0xff, 0x2e, 0x3e,
        0x8b, 0xcc, 0x67, 0x00, 0x0c, 0x57, 0xfa, 0x41, 0xf4, 0x35, 0x64, 0x00, 0x00, 0x64, 0xbf,
        0xd7, 0x8f, 0xa4, 0x43, 0x19, 0xc0, 0x03, 0x1d, 0xfe, 0xcd, 0x7d, 0x53, 0xd8, 0xce, 0x00,
        0x16, 0x6f, 0xf6, 0x83, 0xeb, 0x04, 0xc6, 0x70, 0x00, 0xb0, 0x40, 0x01, 0x24, 0x00, 0x02,
        0x4e, 0xa5, 0x26, 0xe6, 0x53, 0xd4, 0x81, 0x01, 0x01, 0x80, 0x03, 0x00, 0x80, 0x04, 0x98,
        0xfb, 0xf3, 0xb8, 0xb8, 0xc2, 0x49, 0x10, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, 0xb4,
        0xf4, 0x35, 0x84, 0x00, 0xa8, 0x01, 0x02, 0x80, 0x01, 0x24, 0x81, 0x04, 0x03, 0x01, 0x00,
        0x00, 0x80, 0x01, 0x25, 0x81, 0x05, 0x04, 0x01, 0x90, 0x1a, 0x25, 0x80, 0x80, 0x83, 0xdd,
        0xe9, 0xdd, 0x00, 0x4a, 0xc1, 0xa7, 0xfd, 0x3e, 0x0e, 0x9d, 0xb9, 0x76, 0x29, 0x5d, 0xed,
        0xeb, 0xd8, 0x62, 0x21, 0x89, 0xc2, 0x15, 0x78, 0xdf, 0xf4, 0xe8, 0xd6, 0xc1, 0x9e, 0x31,
        0xac, 0x80, 0x80, 0xec, 0xfa, 0x1c, 0x10, 0xde, 0xae, 0xea, 0x93, 0x5f, 0x69, 0x4a, 0xd2,
        0xe8, 0xe4, 0xe5, 0x96, 0xc0, 0xb7, 0x2d, 0x10, 0xb0, 0xc7, 0x87, 0x44, 0x58, 0x65, 0xdc,
        0x7d, 0xec, 0xc5, 0xf4, 0x7e, 0x7a, 0x93, 0x89, 0x71, 0xdb, 0xa1, 0x79, 0xc4, 0xc4, 0x3c,
        0x6b, 0x55, 0xf1, 0xc3, 0x27, 0x33, 0xad, 0x35, 0x09, 0xe5, 0x5d, 0x9f, 0x0d, 0xa2, 0xe1,
        0x4c, 0x8b, 0x37, 0xa4, 0x3b, 0x46, 0xb7, 0x80, 0x82, 0x43, 0x73, 0x00, 0xa4, 0xb7, 0x76,
        0x33, 0x90, 0xab, 0xfa, 0x58, 0xac, 0x1a, 0x29, 0x0a, 0x61, 0x63, 0xfa, 0xa8, 0xe9, 0x4c,
        0xfb, 0xf5, 0x97, 0x5a, 0x8b, 0xfe, 0xae, 0xbb, 0x96, 0x45, 0xf3, 0x9d, 0x16, 0x70, 0xab,
        0x65, 0x4e, 0x0e, 0x0f, 0xf7, 0xca, 0x4c, 0x15, 0xf8, 0xd8, 0xb8, 0x5e, 0xc9, 0x8d, 0x61,
        0x0d, 0x93, 0xca, 0xa7, 0x5f, 0x87, 0x5e, 0xc9, 0xf0, 0x5f, 0xa5, 0x44, 0x6f,
    ];

    #[test]
    fn roundtrip() {
        let (eth, meta) = wifi_to_ethernet(&WIFI_FRAME);
        let wifi_back = ethernet_to_wifi(&eth, &meta);
        assert_eq!(
            wifi_back.as_slice(),
            &WIFI_FRAME[..],
            "round-trip 802.11 -> Ethernet -> 802.11 must be lossless",
        );
    }
}
