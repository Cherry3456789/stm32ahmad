import time
import usb.core
import usb.util
import usb.backend.libusb1

DFU_DETACH    = 0x00
DFU_DNLOAD    = 0x01
DFU_UPLOAD    = 0x02
DFU_GETSTATUS = 0x03
DFU_CLRSTATUS = 0x04
DFU_GETSTATE  = 0x05
DFU_ABORT     = 0x06

STATE_APP_IDLE           = 0
STATE_APP_DETACH         = 1
STATE_DFU_IDLE           = 2
STATE_DFU_DNLOAD_SYNC    = 3
STATE_DFU_DNBUSY         = 4
STATE_DFU_DNLOAD_IDLE    = 5
STATE_DFU_MANIFEST_SYNC  = 6
STATE_DFU_MANIFEST       = 7
STATE_DFU_MANIFEST_WAIT  = 8
STATE_DFU_UPLOAD_IDLE    = 9
STATE_DFU_ERROR          = 10

STATE_NAMES = {
    0: "appIDLE", 1: "appDETACH", 2: "dfuIDLE",
    3: "dfuDNLOAD-SYNC", 4: "dfuDNBUSY", 5: "dfuDNLOAD-IDLE",
    6: "dfuMANIFEST-SYNC", 7: "dfuMANIFEST", 8: "dfuMANIFEST-WAIT-RESET",
    9: "dfuUPLOAD-IDLE", 10: "dfuERROR",
}

STATUS_OK             = 0x00
STATUS_ERR_TARGET     = 0x01
STATUS_ERR_FILE       = 0x02
STATUS_ERR_WRITE      = 0x03
STATUS_ERR_ERASE      = 0x04
STATUS_ERR_CHECK      = 0x05
STATUS_ERR_PROG       = 0x06
STATUS_ERR_VERIFY     = 0x07
STATUS_ERR_ADDRESS    = 0x08
STATUS_ERR_STALLEDPKT = 0x0F

STATUS_NAMES = {
    0x00: "OK", 0x01: "errTARGET", 0x02: "errFILE", 0x03: "errWRITE",
    0x04: "errERASE", 0x05: "errCHECK_ERASED", 0x06: "errPROG",
    0x07: "errVERIFY", 0x08: "errADDRESS", 0x0F: "errSTALLEDPKT",
}

CMD_SET_ADDRESS  = 0x21   
CMD_ERASE_PAGE   = 0x41   
CMD_MASS_ERASE   = 0x41   
CMD_READ_UNPROT  = 0x92   

TRANSFER_SIZE = 2048

RT_HOST_TO_IFACE = 0x21   
RT_IFACE_TO_HOST = 0xA1   


class DFUError(Exception):
    pass


class DFUFlasher:
    def __init__(self, vid: int = 0x0483, pid: int = 0xDF11):
        self.vid = vid
        self.pid = pid
        self.dev: usb.core.Device | None = None

    def _ctrl_out(self, request: int, value: int, data: bytes | list) -> int:
        return self.dev.ctrl_transfer(
            RT_HOST_TO_IFACE, request, value, 0, data, timeout=5000
        )

    def _ctrl_in(self, request: int, value: int, length: int) -> bytes:
        return bytes(
            self.dev.ctrl_transfer(
                RT_IFACE_TO_HOST, request, value, 0, length, timeout=50000
            )
        )
        
    def _get_status(self) -> tuple[int, int, int]:
        resp = self._ctrl_in(DFU_GETSTATUS, 0, 6)
        if len(resp) < 6:
            raise DFUError(f"DFU_GETSTATUS returned only {len(resp)} bytes")
        b_status      = resp[0]
        poll_timeout  = resp[1] | (resp[2] << 8) | (resp[3] << 16)
        b_state       = resp[4]
        return b_status, b_state, poll_timeout

    def _clear_status(self):
        self._ctrl_out(DFU_CLRSTATUS, 0, [])

    def _abort(self):
        self._ctrl_out(DFU_ABORT, 0, [])

    def _dnload(self, block_num: int, data: bytes):
        self._ctrl_out(DFU_DNLOAD, block_num, data)

    def _wait_for_state(
        self,
        target_state: int,
        max_polls: int = 200,
        extra_timeout_s: float = 30.0,
    ):
        deadline = time.monotonic() + extra_timeout_s
        for _ in range(max_polls):
            status, state, poll_ms = self._get_status()

            if status != STATUS_OK:
                status_name = STATUS_NAMES.get(status, f"0x{status:02X}")
                state_name  = STATE_NAMES.get(state, str(state))
                if state == STATE_DFU_ERROR:
                    self._clear_status()
                raise DFUError(
                    f"DFU error: status={status_name}, state={state_name}"
                )

            if state == target_state:
                return

            if state == STATE_DFU_ERROR:
                self._clear_status()
                raise DFUError("Device entered dfuERROR (cleared)")

            if time.monotonic() > deadline:
                break

            time.sleep(max(poll_ms, 10) / 1000.0)

        raise DFUError(
            f"Timeout waiting for state {STATE_NAMES.get(target_state, target_state)}"
        )


    def _set_address_pointer(self, address: int):
        cmd = bytes([
            CMD_SET_ADDRESS,
            (address)       & 0xFF,
            (address >> 8)  & 0xFF,
            (address >> 16) & 0xFF,
            (address >> 24) & 0xFF,
        ])
        self._dnload(0, cmd)
        self._wait_for_state(STATE_DFU_DNLOAD_IDLE)

    def _mass_erase(self):
        print("[DFU] Starting mass erase (this may take ~20 s)...", end="", flush=True)
        cmd = bytes([CMD_MASS_ERASE])   
        self._dnload(0, cmd)
        self._wait_for_state(
            STATE_DFU_DNLOAD_IDLE,
            max_polls=400,
            extra_timeout_s=45.0,
        )
        print(" done.")

    def _find_device(self, timeout_s: float = 20.0) -> usb.core.Device:
        print(
            f"[DFU] Waiting for {self.vid:04X}:{self.pid:04X} in DFU mode",
            end="",
            flush=True,
        )
        backend  = usb.backend.libusb1.get_backend()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            dev = usb.core.find(
                idVendor=self.vid, idProduct=self.pid, backend=backend
            )
            if dev is not None:
                print(f" — found on bus {dev.bus}, address {dev.address}.")
                return dev
            print(".", end="", flush=True)
            time.sleep(0.5)

        raise DFUError(
            f"\nDFU device {self.vid:04X}:{self.pid:04X} not found after "
            f"{timeout_s:.0f} s.\n"
            "  → Hold BOOT0 HIGH and press RESET on the Discovery board."
        )

    def _acquire(self):
        try:
            if self.dev.is_kernel_driver_active(0):
                self.dev.detach_kernel_driver(0)
        except (usb.core.USBError, NotImplementedError):
            pass

        self.dev.set_configuration()
        usb.util.claim_interface(self.dev, 0)

        _, state, _ = self._get_status()
        if state == STATE_DFU_ERROR:
            print("[DFU] Clearing previous error state...")
            self._clear_status()
            _, state, _ = self._get_status()

        if state not in (STATE_DFU_IDLE, STATE_DFU_DNLOAD_IDLE):
            print(f"[DFU] Unexpected state {STATE_NAMES.get(state, state)}, aborting...")
            self._abort()
            self._wait_for_state(STATE_DFU_IDLE)

    def _leave_dfu(self, app_base: int = 0x08000000):
        print("[DFU] Triggering manifest / device reset...")
        self._set_address_pointer(app_base)

        try:
            self._dnload(0, b"")
            self._get_status()
        except usb.core.USBError:
            pass 

        time.sleep(0.5)

    def flash(self, bin_path: str, base_addr: int = 0x08000000):
        with open(bin_path, "rb") as f:
            firmware = f.read()

        total_bytes  = len(firmware)
        total_blocks = (total_bytes + TRANSFER_SIZE - 1) // TRANSFER_SIZE
        print(
            f"[DFU] Firmware : {bin_path}\n"
            f"[DFU] Size     : {total_bytes} bytes  ({total_blocks} blocks × {TRANSFER_SIZE} B)\n"
            f"[DFU] Base     : 0x{base_addr:08X}"
        )

        self.dev = self._find_device()
        self._acquire()

        self._mass_erase()

        self._set_address_pointer(base_addr)

        print(f"[DFU] Downloading firmware...")
        for block_idx in range(total_blocks):
            start   = block_idx * TRANSFER_SIZE
            chunk   = firmware[start : start + TRANSFER_SIZE]
            blk_num = block_idx + 2       

            self._dnload(blk_num, chunk)
            self._wait_for_state(STATE_DFU_DNLOAD_IDLE)

            pct = 100 * (block_idx + 1) // total_blocks
            bar = "#" * (pct // 5) + "." * (20 - pct // 5)
            print(
                f"\r[DFU]  [{bar}] {pct:3d}%  block {block_idx+1}/{total_blocks}",
                end="",
                flush=True,
            )

        print("\n[DFU] Download complete.")

        self._leave_dfu(app_base=base_addr)

        usb.util.release_interface(self.dev, 0)
        print("[DFU] Flash successful — MCU is rebooting into application.")
