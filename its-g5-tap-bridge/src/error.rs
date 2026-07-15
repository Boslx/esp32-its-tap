use thiserror::Error;

#[derive(Error, Debug)]
pub enum Error {
    #[error("{context}: {source}")]
    Io {
        context: &'static str,
        #[source]
        source: std::io::Error,
    },

    #[error("Serial port {port} @ {baud} baud: {source}")]
    Serial {
        port: String,
        baud: u32,
        #[source]
        source: serialport::Error,
    },

    #[error("Failed to decode protobuf message from serial: {0}")]
    ProstDecode(#[from] prost::DecodeError),

    #[error("Failed to encode protobuf message for serial: {0}")]
    ProstEncode(#[from] prost::EncodeError),
}

impl From<std::io::Error> for Error {
    fn from(source: std::io::Error) -> Self {
        Error::Io {
            context: "I/O operation failed",
            source,
        }
    }
}

pub const MAX_PROTOBUF_SIZE: u32 = 4096;

pub type Result<T> = std::result::Result<T, Error>;
