// TODO: present a more Rust-y interface
pub use crate::sys::ipc_get_msg;
pub use crate::sys::ipc_port_connect_async;
pub use crate::sys::ipc_put_msg;
pub use crate::sys::ipc_read_msg;
pub use crate::sys::ipc_send_msg;

pub use crate::sys::iovec_kern;
pub use crate::sys::ipc_msg_info;
pub use crate::sys::ipc_msg_kern;

pub use crate::sys::zero_uuid;
pub use crate::sys::IPC_CONNECT_WAIT_FOR_PORT;
