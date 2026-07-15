use prost::Message;

use crate::error::{Result, MAX_PROTOBUF_SIZE};
use crate::its_proto::{FromBoard, ToBoard};

const MAGIC_1: u8 = 0xBE;
const MAGIC_2: u8 = 0xEF;

/// Encode a `ToBoard` protobuf message into a framed wire-format buffer:
/// `[0xBE, 0xEF, u32 LE length, protobuf bytes]`.
pub fn encode_to_board(msg: &ToBoard) -> Result<Vec<u8>> {
    let pb_data = prost::Message::encode_to_vec(msg);
    let len = pb_data.len() as u32;
    let mut frame = Vec::with_capacity(2 + 4 + pb_data.len());
    frame.push(MAGIC_1);
    frame.push(MAGIC_2);
    frame.extend_from_slice(&len.to_le_bytes());
    frame.extend_from_slice(&pb_data);
    Ok(frame)
}

/// State machine for decoding framed protobuf messages from a byte stream.
///
/// Matches the C implementation in `src/serial_protocol.c`:
/// searches for the `0xBE 0xEF` magic marker, reads a 4-byte LE length,
/// then collects that many bytes of protobuf payload. Handles false sync
/// (only first magic byte matches), invalid lengths, and partial frames.
pub struct FrameDecoder {
    state: RxState,
    expected_len: u32,
    bytes_read: u32,
    pb_buf: Vec<u8>,
    pending: Vec<FromBoard>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RxState {
    Sync1,
    Sync2,
    Length,
    Data,
}

impl FrameDecoder {
    pub fn new() -> Self {
        Self {
            state: RxState::Sync1,
            expected_len: 0,
            bytes_read: 0,
            pb_buf: Vec::new(),
            pending: Vec::new(),
        }
    }

    /// Feed raw bytes into the decoder. Returns all complete `FromBoard`
    /// messages parsed so far. Partial data is buffered internally.
    pub fn push(&mut self, data: &[u8]) -> Result<Vec<FromBoard>> {
        self.pending.clear();

        for &byte in data {
            match self.state {
                RxState::Sync1 => {
                    if byte == MAGIC_1 {
                        self.state = RxState::Sync2;
                    }
                }
                RxState::Sync2 => {
                    if byte == MAGIC_2 {
                        self.state = RxState::Length;
                        self.expected_len = 0;
                        self.bytes_read = 0;
                    } else {
                        self.state = RxState::Sync1;
                    }
                }
                RxState::Length => {
                    self.expected_len |= (byte as u32) << (self.bytes_read * 8);
                    self.bytes_read += 1;
                    if self.bytes_read >= 4 {
                        if self.expected_len == 0 || self.expected_len > MAX_PROTOBUF_SIZE {
                            self.state = RxState::Sync1;
                        } else {
                            self.bytes_read = 0;
                            self.pb_buf = vec![0u8; self.expected_len as usize];
                            self.state = RxState::Data;
                        }
                    }
                }
                RxState::Data => {
                    self.pb_buf[self.bytes_read as usize] = byte;
                    self.bytes_read += 1;
                    if self.bytes_read >= self.expected_len {
                        let msg = FromBoard::decode(self.pb_buf.as_slice())?;
                        self.pending.push(msg);
                        self.state = RxState::Sync1;
                    }
                }
            }
        }

        Ok(std::mem::take(&mut self.pending))
    }
}

impl Default for FrameDecoder {
    fn default() -> Self {
        Self::new()
    }
}
