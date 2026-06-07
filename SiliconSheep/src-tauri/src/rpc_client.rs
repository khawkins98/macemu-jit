// C2.0 RPC client — connects to SheepShaver's UDS server for sub-16ms IPC.
// Wire protocol matches rpc_unix.cpp: network-byte-order int32 framing.

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::time::Duration;

const RPC_MESSAGE_START: i32 = -3000;
const RPC_MESSAGE_END: i32 = -3001;
const RPC_MESSAGE_ACK: i32 = -3002;
const RPC_TYPE_INVALID: i32 = 0;
const RPC_TYPE_INT32: i32 = -2002;
const RPC_TYPE_UINT32: i32 = -2003;
const RPC_TYPE_STRING: i32 = -2004;
const RPC_TYPE_ARRAY: i32 = -2005;

// Method IDs matching rpc.h
pub const METHOD_INPUT_LOCKOUT: i32 = 11;
pub const METHOD_FRAMESKIP: i32 = 12;
pub const METHOD_MOUSE_GRAB: i32 = 13;
pub const METHOD_GET_STATS: i32 = 14;
pub const METHOD_READ_MEMORY: i32 = 15;
pub const METHOD_DUMP_REGISTERS: i32 = 16;

pub struct RpcClient {
    stream: UnixStream,
}

impl RpcClient {
    pub fn connect(socket_path: &str) -> Result<Self, String> {
        let path = Path::new(socket_path);
        let stream = UnixStream::connect(path)
            .map_err(|e| format!("RPC connect failed: {}", e))?;
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .ok();
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .ok();
        Ok(RpcClient { stream })
    }

    pub fn connect_from_vm(vm_dir: &Path) -> Result<Self, String> {
        let socket_file = vm_dir.join("rpc_socket");
        let path = std::fs::read_to_string(&socket_file)
            .map_err(|e| format!("Cannot read rpc_socket: {}", e))?;
        Self::connect(path.trim())
    }

    fn write_i32(&mut self, val: i32) -> Result<(), String> {
        self.stream
            .write_all(&val.to_be_bytes())
            .map_err(|e| format!("RPC write: {}", e))
    }

    fn read_i32(&mut self) -> Result<i32, String> {
        let mut buf = [0u8; 4];
        self.stream
            .read_exact(&mut buf)
            .map_err(|e| format!("RPC read: {}", e))?;
        Ok(i32::from_be_bytes(buf))
    }

    fn read_string(&mut self) -> Result<String, String> {
        let len = self.read_i32()? as usize;
        let mut buf = vec![0u8; len];
        self.stream
            .read_exact(&mut buf)
            .map_err(|e| format!("RPC read string: {}", e))?;
        // Read trailing null
        let mut null = [0u8; 1];
        self.stream.read_exact(&mut null).ok();
        Ok(String::from_utf8_lossy(&buf).to_string())
    }

    /// Send a method with an int32 argument, wait for ACK
    pub fn invoke_int32(&mut self, method: i32, val: i32) -> Result<(), String> {
        self.write_i32(RPC_MESSAGE_START)?;
        self.write_i32(method)?;
        self.write_i32(RPC_TYPE_INT32)?;
        self.write_i32(val)?;
        self.write_i32(RPC_TYPE_INVALID)?;
        self.write_i32(RPC_MESSAGE_END)?;

        // Wait for ACK
        let ack = self.read_i32()?;
        if ack != RPC_MESSAGE_ACK {
            return Err(format!("Expected ACK, got {}", ack));
        }
        Ok(())
    }

    /// Send a method with no arguments, receive a string reply
    pub fn invoke_get_string(&mut self, method: i32) -> Result<String, String> {
        self.write_i32(RPC_MESSAGE_START)?;
        self.write_i32(method)?;
        self.write_i32(RPC_TYPE_INVALID)?;
        self.write_i32(RPC_MESSAGE_END)?;

        // Read reply: RPC_MESSAGE_REPLY, type=STRING, data, RPC_MESSAGE_END, RPC_MESSAGE_ACK
        let reply_marker = self.read_i32()?;
        if reply_marker == RPC_MESSAGE_ACK {
            return Ok(String::new()); // no reply data, just ACK
        }
        // reply_marker should be RPC_MESSAGE_REPLY (-3003)
        let type_tag = self.read_i32()?;
        let result = if type_tag == RPC_TYPE_STRING {
            self.read_string()?
        } else {
            String::new()
        };
        // Read through to MESSAGE_END + ACK
        loop {
            let v = self.read_i32()?;
            if v == RPC_MESSAGE_ACK {
                break;
            }
        }
        Ok(result)
    }

    /// Send READ_MEMORY(addr, len) and receive bytes back as hex string
    pub fn invoke_read_memory(&mut self, addr: u32, len: u32) -> Result<Vec<u8>, String> {
        self.write_i32(RPC_MESSAGE_START)?;
        self.write_i32(METHOD_READ_MEMORY)?;
        self.write_i32(RPC_TYPE_UINT32)?;
        self.write_i32(addr as i32)?;
        self.write_i32(RPC_TYPE_UINT32)?;
        self.write_i32(len as i32)?;
        self.write_i32(RPC_TYPE_INVALID)?;
        self.write_i32(RPC_MESSAGE_END)?;

        let reply_marker = self.read_i32()?;
        if reply_marker == RPC_MESSAGE_ACK {
            return Ok(Vec::new());
        }
        let type_tag = self.read_i32()?;
        if type_tag == RPC_TYPE_ARRAY {
            let _elem_type = self.read_i32()?;
            let count = self.read_i32()? as usize;
            let mut bytes = vec![0u8; count];
            self.stream.read_exact(&mut bytes).map_err(|e| format!("read bytes: {}", e))?;
            loop {
                let v = self.read_i32()?;
                if v == RPC_MESSAGE_ACK { break; }
            }
            Ok(bytes)
        } else {
            loop {
                let v = self.read_i32()?;
                if v == RPC_MESSAGE_ACK { break; }
            }
            Err("Unexpected reply type".to_string())
        }
    }
}
