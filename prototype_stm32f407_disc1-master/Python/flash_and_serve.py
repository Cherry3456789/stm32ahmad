import sys
import time
import struct
import argparse
import threading
from typing import Union #New
import serial
import serial.tools.list_ports

from dfu_util import DFUFlasher, DFUError
from draw import get_drawing_buffer #New

START_BYTE     = 0xAA
FRAME_HDR_LEN  = 7       

STM32_DFU_VID  = 0x0483
STM32_DFU_PID  = 0xDF11
STM32_CDC_VID  = 0x0483
STM32_CDC_PID  = 0x5740   

def find_cdc_port(
    vid: int = STM32_CDC_VID,
    pid: int = STM32_CDC_PID,
    timeout_s: float = 30.0,
) -> str:
    print(
        f"[HOST] Waiting for CDC device {vid:04X}:{pid:04X} ...",
        end="",
        flush=True,
    )
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for port in serial.tools.list_ports.comports():
            if port.vid == vid and port.pid == pid:
                print(f" found → {port.device}")
                time.sleep(0.5)   
                return port.device
        print(".", end="", flush=True)
        time.sleep(1.0)
    raise TimeoutError(
        f"\nCDC device {vid:04X}:{pid:04X} not found within {timeout_s:.0f} s.\n"
        "  → Check USB cable and that the firmware initialises USB_OTG_FS."
    )

class DataServer:
    _JUMP_SIGNAL = "__JUMP_BOOTLOADER__"

    def __init__(self, port: str, source: Union[str, bytes, bytearray], baud: int = 115200, mode: str = "slave"):
        self.port = port
        self.baud = baud
        self.mode = mode.lower()
        self._stop = threading.Event()
        self._jump = threading.Event()
        self._ack_received = False
        self._expected_offset = 0
        self._expected_length = 0
        self._handshake_done = False
        self._last_send_time = 0
        self._timeout = 5.0
        self._wait_time = 0.0

        if isinstance(source, str):
            print(f"[HOST] Loading data from file: {source}")
            with open(source, "rb") as f:
                self.data = f.read()
            print(f"[HOST] Data size: {len(self.data)} bytes")
        else:
            print(f"[HOST] Loading data from buffer ({len(source)} bytes)")
            self.data = bytes(source)
            
    def run(self, chunk_size: int = 512) -> str | None:
        print(f"[HOST] Opening {self.port} @ {self.baud} baud [MODE: {self.mode.upper()}]")
        ser = serial.Serial(self.port, self.baud, timeout=0.01)
        
        rx_buf = bytearray()
        string_buf = bytearray()
        offset = 0
        
        try:
            while not self._stop.is_set():
                now = time.time()
            
                if self._wait_time > 0:
                    if now > self._wait_time:
                        self._wait_time = 0
                        if self._ack_received == "FINISH": 
                            break
                            
                elif self.mode == "master":
                    if not self._handshake_done:
                        if (now - self._last_send_time) > self._timeout:
                            ser.write(struct.pack("<BIH", START_BYTE, 0, 0))
                            self._last_send_time = now
                            print("[HOST] Sending Handshake Sync")
                    
                    elif offset < len(self.data):
                        if self._ack_received is False:
                            self._expected_offset = offset
                            length = min(chunk_size, len(self.data) - offset)
                            self._expected_ln = length
                            self._last_send_time = now
                            packet = self._build_response(offset, length)
                            ser.write(packet)
                            print(f"[HOST] Sent offset 0x{offset:08X}. Waiting for ACK...")
                            self._ack_received = "WAITING" 
                        elif self._ack_received == "WAITING":
                            if (now - self._last_send_time) > self._timeout:
                                print(f"[HOST] Timeout at 0x{offset:08X}")
                                break
                    elif self._ack_received == "FINISH":
                        break
    
                raw = ser.read(4096)
                if raw:
                    rx_buf.extend(raw)
                    self._process_buffer(rx_buf, string_buf, ser)
                    
                if self.mode == "master" and self._ack_received == "RECEIVED":          
                    offset += length 
                    print("[HOST] Ack Received.")
                    self._ack_received = False 
                    if offset >= len(self.data):
                        print("[HOST] All data sent successfully.")
                        self._wait_time = now + 2.0
                        self._ack_received = "FINISH"
                        
        except KeyboardInterrupt:
            print("\n[HOST] Interrupted.")
        finally:
            ser.close()
        return self._JUMP_SIGNAL if self._jump.is_set() else None

    @staticmethod
    def _parse_request(raw: bytes) -> tuple[int, int] | None:
        if len(raw) < FRAME_HDR_LEN or raw[0] != START_BYTE:
            return None
        offset = struct.unpack_from("<I", raw, 1)[0]
        length = struct.unpack_from("<H", raw, 5)[0]
        return offset, length

    def _build_response(self, offset: int, length: int) -> bytes:
        chunk = self.data[offset : offset + length]
        header = struct.pack("<BIH", START_BYTE, offset, len(chunk))
        return header + chunk
        
    def _process_buffer(self, rx_buf: bytearray, string_buf: bytearray, ser: serial.Serial):
        while rx_buf:
            head = rx_buf[0]
            if head == START_BYTE:
                if len(rx_buf) < FRAME_HDR_LEN:
                    break   

                parsed = self._parse_request(bytes(rx_buf[:FRAME_HDR_LEN]))
                if parsed is None:
                    string_buf.append(rx_buf.pop(0))
                    continue

                offset, length = parsed
                del rx_buf[:FRAME_HDR_LEN]

                if offset == 0 and length == 0:
                    if not self._handshake_done:
                        self._handshake_done = True
                        print("[HOST] Handshake Successful.")
                        if self.mode == "master":
                            self._wait_time = time.time() + 1.0
                        if self.mode == "slave":
                            ser.write(struct.pack("<BIH", START_BYTE, 0, 0)) 
                    continue

                if self.mode == "master" and offset == self._expected_offset:#TODO: and length == self._expected_length: why doesntt work?
                    self._ack_received = "RECEIVED"

                elif self.mode == "slave" and self._handshake_done:
                    response = self._build_response(offset, length)
                    if response:
                        ser.write(response)
                    print(
                        f"[HOST] → chunk  offset=0x{offset:08X}  "
                        f"len={length}  ({length} B sent)"
                    )
            else:
                byte = rx_buf.pop(0)
                if byte == 0x0A:   
                    msg = string_buf.decode("utf-8", errors="replace").rstrip("\r")
                    string_buf.clear()
                    if msg: self._handle_debug_string(msg, ser)
                elif byte != 0x0D:
                    string_buf.append(byte)

    def _handle_debug_string(self, msg, ser):
        print(f"[STM32] {msg.strip()}")
    def stop(self):
        self._stop.set()


def do_flash(
    firmware_path: str,
    base_addr: int,
    dfu_vid: int,
    dfu_pid: int,
):
    print("\n" + "=" * 60)
    print("  PHASE 1 — DFU FLASH")
    print("=" * 60)
    try:
        flasher = DFUFlasher(vid=dfu_vid, pid=dfu_pid)
        flasher.flash(firmware_path, base_addr)
    except DFUError as exc:
        print(f"\n[HOST] DFU Error: {exc}", file=sys.stderr)
        sys.exit(1)

def do_serve(
    data_source: Union[str, bytes, bytearray],
    baud: int,
    cdc_vid: int,
    cdc_pid: int,
    mode: str = "slave"
) -> str | None:
    print("\n" + "=" * 60)
    print(f"  PHASE 2 — CDC DATA SERVER [{mode.upper()} MODE]")
    print("=" * 60)
    port = find_cdc_port(vid=cdc_vid, pid=cdc_pid)
    if not port:
        print("[ERROR] Could not find STM32 CDC Port. Is it plugged in?")
        return None
    server = DataServer(port=port, source=data_source, baud=baud, mode=mode)
    return server.run()

def do_jump(
    baud: int,
    cdc_vid: int,
    cdc_pid: int,
) -> str | None:
    print("\n" + "=" * 60)
    print("  PHASE 0 — Jump to Bootloader")
    print("=" * 60)
    port   = find_cdc_port(vid=cdc_vid, pid=cdc_pid)
    ser = serial.Serial(port, baud, timeout=0.05)
    ser.write(b"CMD:JUMP_BOOTLOADER\n")
    print("Jump to bootloader command was sent \n")
    time.sleep(3.0) 
    #TODO: reset receive buffer if ser.in_waiting > 0:
     #   ser.read(ser.in_waiting)
    
def main():
    parser = argparse.ArgumentParser(
        description="Flash STM32 via DFU, then serve binary data over USB CDC.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--firmware",  help="Path to firmware .bin file")
    parser.add_argument("--data", help="Path to binary data file or buffer source")
    parser.add_argument(
        "--mode",
        choices=["master", "slave"],
        default="slave",
        help="Server mode: 'slave' (waits for requests) or 'master' (pushes data). Default: slave",
    )
    parser.add_argument(
        "--base-addr",
        default="0x08000000",
        help="Flash base address  (default: 0x08000000)",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="CDC baud rate  (default: 115200)",
    )
    parser.add_argument(
        "--dfu-vid",
        type=lambda x: int(x, 16),
        default=STM32_DFU_VID,
        metavar="VID",
        help=f"DFU USB Vendor ID  (default: {STM32_DFU_VID:04X})",
    )
    parser.add_argument(
        "--dfu-pid",
        type=lambda x: int(x, 16),
        default=STM32_DFU_PID,
        metavar="PID",
        help=f"DFU USB Product ID  (default: {STM32_DFU_PID:04X})",
    )
    parser.add_argument(
        "--cdc-vid",
        type=lambda x: int(x, 16),
        default=STM32_CDC_VID,
        metavar="VID",
        help=f"CDC USB Vendor ID  (default: {STM32_CDC_VID:04X})",
    )
    parser.add_argument(
        "--cdc-pid",
        type=lambda x: int(x, 16),
        default=STM32_CDC_PID,
        metavar="PID",
        help=f"CDC USB Product ID  (default: {STM32_CDC_PID:04X})",
    )
   
    parser.add_argument(
        "--jump",
        action="store_true",
        help="Jump to Bootloader command",
    )
    
    parser.add_argument(
        "--no-flash",
        action="store_true",
        help="Skip DFU flash (device already running firmware)",
    )
    parser.add_argument(
        "--flash-only",
        action="store_true",
        help="Flash firmware and exit without starting data server",
    )

    parser.add_argument(
        "--draw", 
        action="store_true", 
        help="Open UI to draw a digit and send it (Master mode only)"
    )

    args       = parser.parse_args()
    base_addr  = int(args.base_addr, 16)

    if not args.no_flash and not args.firmware:
        parser.error("--firmware is required unless --no-flash is used.")
    
    if args.no_flash and args.jump:
        parser.error("Conflicting arguments: Use either--no-flash OR --jump, not both.")
        
    if not args.flash_only:
        if not args.draw and not args.data:
            parser.error("You must provide --data <file> or use --draw to serve data.")

        if args.draw and args.data:
            parser.error("Conflicting data sources: Use either --data OR --draw, not both.")

        if args.draw and args.mode != "master":
            parser.error("--draw can only be used in --mode master.")

    if args.jump:
        do_jump(args.baud, args.cdc_vid, args.cdc_pid)
    
    if not args.no_flash:
        do_flash(args.firmware, base_addr, args.dfu_vid, args.dfu_pid)

    if args.flash_only:
        print("[HOST] --flash-only specified.  Done.")
        return
    else:
        if args.draw:
            print("[HOST] Opening drawing canvas...")
            drawing_array = get_drawing_buffer()
            if drawing_array is None:
                print("[HOST] Drawing cancelled. Exiting.")
                return
            data_to_serve = drawing_array.tobytes()
        else:
            data_to_serve = args.data
        
    while True:
        signal = do_serve(data_to_serve, args.baud, args.cdc_vid, args.cdc_pid, mode=args.mode)

        if signal == DataServer._JUMP_SIGNAL:
            print(
                "\n[HOST] STM32 has jumped to bootloader."
                "  Waiting for DFU device to appear..."
            )
            time.sleep(2.0)
            do_flash(args.firmware, base_addr, args.dfu_vid, args.dfu_pid)
        else:
            break   

    print("[HOST] Goodbye.")


if __name__ == "__main__":
    main()
