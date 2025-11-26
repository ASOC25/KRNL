# Mini tutorial TLDR:

A ver, esto es muy fácil:

1. Clona el repositorio
   ```bash
   git clone git@github.com:ASOC25/KRNL.git
   cd KRNL
    ```

    Si quieres navegar por las ramas:
    ```bash
    git checkout <branch-name>
    ```

2. Comprueba que tienes las herramientas necesarias instaladas, principalmente:
    - `build-essential` (o equivalente)
    - `nasm`
    - `qemu-system`
    - `gdb`

    Si te falta alguna, chatgpt te puede ayudar a instalarlas jeje

3. Comprueba que el kernel compila:

    ```bash
    make kernel
    ```

4. Comprueba que la imagen de arranque se crea:

    ```bash
    make image
    ```

Llegado este punto ya tienes el kernel compilado y la imagen de arranque creada. A currar!

Normalmente querrás debuguear, así que antes de nada abre una nueva terminal y lanza el script:

    ```bash
    scripts/connect-to-debug.sh
    ```

Este script se quedará esperando a que arranques QEMU para conectarse automáticamente al stub de gdb.
Si te da timeout, no te preocupes, simplemente presiona q y enter para salir de gdb y volver a intentarlo.

Para lanzar QEMU con el kernel y la imagen de arranque, usa:

    ```bash
    make debug
    ```

Deberías ver que QEMU arranca y que gdb se conecta automáticamente y para en el primer breakpoint.

A partir de aquí, puedes usar los comandos habituales de gdb para debuguear tu kernel. Algunos comandos útiles son:

    - `continue` o `c`: para continuar la ejecución
    - `step` o `s`: para ejecutar la siguiente instrucción
    - `stepi` o `si`: para ejecutar la siguiente instrucción (sin entrar en funciones)
    - `next` o `n`: para ejecutar la siguiente línea de código (sin entrar en funciones)
    - `break <función>` o `b <función>`: para poner un breakpoint en una función específica
    - `break *0x<dirección>`: para poner un breakpoint en una dirección específica
    - `disassemble _start`: para ver el desensamblado de la función `_start` (cualquier función sirve)
    - `bt`: para ver el backtrace de llamadas
    - `info locals`: para ver las variables locales en el contexto actual
    - `info registers`: para ver el estado de los registros
    - `info threads`: para ver los hilos
    - `thread <número>`: para cambiar al hilo número `<número>`
    - `info frame`: para ver información del frame actual 
    - `list` o `l`: para ver el código fuente alrededor de la línea actual (si tienes los símbolos)
    - `set $rip=0x<dirección>`: para cambiar el puntero de instrucción a una dirección específica (cualquier registro o variable sirve)
    - `awatch <dirección>`: para poner un watchpoint que pare cuando se acceda a una dirección específica (tb vale  cualquier variable)
    - `rwatch <dirección>`: para poner un watchpoint que pare cuando se lea una dirección específica (tb vale  cualquier variable)
    - `watch <variable>`: para poner un watchpoint que pare cuando una variable cambie (tb  vale cualquier direccion)
    - `p <variable>`: para imprimir el valor de una variable (puedes hacer *variable si es un puntero)
    - `p/x $<registro>`: para imprimir el valor de un registro en hexadecimal
    - `x/10xb $rsp`: para examinar la memoria alrededor del puntero de pila
    - `x/10xg <dirección>`: para examinar memoria en una dirección específica (10 elementos de tamaño 'g' - 8 bytes impresos en hexadecimal)
    - `quit` o `q`: para salir de gdb

Otro truco es teclear la macro de abajo para que cada vez que gdb pare, imprima las siguientes 10 instrucciones a partir del puntero de instrucción.

    ```bash
    define hook-stop
        x/10ig $rip
        //Puedes añadir más comandos aquí si quieres
    end
    ```

Recuerda tb que tienes una ventana de debug en qemu a la que puedes acceder con `Ctrl+Alt+2`. El comando más útil ahí es `info tlb` para ver las traducciones de direcciones de memoria. Aunque hay muchos otros comandos útiles.

En general hay mil millones de formas de debuguear, así que explora y prueba cosas nuevas!

Si quieres ejecutar sin el debugger, simplemente usa:

    ```bash
    make run
    ```

Si haces algun cambio en los headers (.h) o crees que algo no se ha recompilado bien, puedes hacer una limpieza rápida con:

    ```bash
    make clean
    ```

No hace falta hacer `make kernel` `make image` y luego `make debug`, ya que los objetivos dependen unos de otros y se encargarán de compilar lo que haga falta automáticamente.

#  README

# Minimal UEFI Kernel + Limine Setup

This repository contains a very small freestanding x86_64 kernel built as an ELF and booted via a GPT/FAT EFI System Partition using the Limine boot components (`BOOTX64.EFI`). Helper scripts create a disk image, run it under QEMU with OVMF, and provide debugging conveniences.

## 1. Host Requirements

You need a Linux host with the following packages/tools (Debian/Ubuntu names shown):

- Build toolchain: `build-essential` (gcc, ld, make) or equivalent cross compiler
- NASM (for any future assembly files) `nasm`
- QEMU system emulator: `qemu-system-x86` (package name may vary)
- OVMF firmware: `ovmf` (provides `OVMF_CODE*.fd`, `OVMF_VARS*.fd`)
- Disk utilities: `parted`, `losetup`, `util-linux` (losetup/mount), `dosfstools` (mkfs.vfat)
- GDB: `gdb` (for debugging)
- Sudo rights (image creation uses loop devices & formatting)

Example install (Debian/Ubuntu):
```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 ovmf parted dosfstools gdb
```

Fedora/RHEL:
```bash
sudo dnf install @development-tools nasm qemu-system-x86 ovmf parted dosfstools gdb
```

Arch:
```bash
sudo pacman -S base-devel nasm qemu-full ovmf parted dosfstools gdb
```

## 2. Repository Layout

```
GNUmakefile          # Build rules
src/                 # Kernel sources
linker.ld            # Linker script
limine/              # Limine-provided EFI binaries & helpers
startup.nsh          # Optional EFI shell script
limine.conf          # Limine configuration file
scripts/             # Helper scripts
  make-efi-img.sh    # Build GPT/FAT EFI image with kernel + limine
  run-qemu.sh        # Run the built image under QEMU + OVMF
  debug-qemu.sh      # Run QEMU + GDB (gdb stub + auto attach)
  cleanup-loops.sh   # Clean up stale loop devices/mounts
build/               # Output directory (kernel.elf, disk.img, etc.)
```

## 3. Make Targets

| Target  | Description |
|---------|-------------|
| `kernel` | Compiles sources & links `build/kernel.elf`. Implicit prerequisite for others. |
| `image`  | Builds `build/disk.img` (GPT + FAT32 ESP) and populates: `EFI/BOOT/BOOTX64.EFI`, `kernel.elf`, `startup.nsh`, `limine.conf`. |
| `run`    | Builds image (if needed) then launches QEMU using `scripts/run-qemu.sh`. |
| `debug`  | Builds image (if needed) then starts QEMU paused with gdb attached using `scripts/debug-qemu.sh`. |
| `clean`  | Remove build artifacts (build directory, images, logs). |
| `distclean` | More thorough clean (same as clean; pass DIST_EXTRAS=yes to script to also remove firmware/limine). |

### Environment Overrides
Most scripts honor environment variables. Examples:
```bash
IMG=build/custom.img IMG_SIZE_MB=128 make image   # Larger image
RAM=1024 SMP=2 make run                           # More RAM & vCPUs
GDB=yes make run                                  # Start QEMU waiting for gdb (manual attach)
NO_GDB=yes make debug                             # Launch only QEMU gdb stub
make clean                                        # Remove build/ and generated images
make distclean                                   # Deeper clean (keeps firmware by default)
```

## 4. Building & Running

Warning, make sure all scripts inside the `scripts/` directory are executable (`chmod +x scripts/*.sh`).

Build the kernel and disk image:
```bash
make image
```
Run it:
```bash
make run
```

Direct script usage:
```bash
scripts/make-efi-img.sh --force
scripts/run-qemu.sh
```

## 5. Debugging

Start QEMU with gdb auto-attached and a breakpoint at `_start`:
```bash
make debug
```
The GDB initialization file `debug.gdb` will:
- Connect to `:1234`
- Set `break _start`
- Continue execution
- Show registers each stop

Manual attach example (if you used `NO_GDB=yes`):
```bash
gdb -ex 'target remote :1234' build/kernel.elf
```

Common helpful GDB commands:
```
disassemble _start
info registers
x/16gx $rsp
bt
```

## 6. Disk Image Details

The image layout (single EFI System Partition):
```
/EFI/BOOT/BOOTX64.EFI
/kernel.elf
/startup.nsh
/limine.conf
```
Created via loop device + `parted` + `mkfs.vfat`. Script: `scripts/make-efi-img.sh`.

Adjust size:
```bash
IMG_SIZE_MB=128 make image
```

## 7. Cleanup Script (Loop Device Recovery)

If image creation was interrupted (Ctrl+C) you may end up with:
- Mounted `mnt-esp` directory that wasn't unmounted
- A loop device still attached to `build/disk.img`

Symptoms:
- `losetup -a` still lists `/dev/loopX` referencing your image
- `mount` shows a lingering mount at `.../mnt-esp`
- Subsequent `make image` fails or hangs

Run the cleanup tool:
```bash
scripts/cleanup-loops.sh
```
Dry run (preview actions):
```bash
DRY_RUN=yes scripts/cleanup-loops.sh
```
Force detach (only if you are sure nothing is using the device):
```bash
FORCE=yes scripts/cleanup-loops.sh
```
Specify a pattern of images:
```bash
IMG_PATTERN='build/disk*.img' scripts/cleanup-loops.sh
```

What it does:
1. Unmounts stale `mnt-esp` if mounted
2. Removes the directory if empty
3. Scans `losetup -a` for loop devices whose backing file matches `IMG_PATTERN`
4. Detaches them if safe (or if `FORCE=yes`)

WARNING: Using `FORCE=yes` can detach loop devices in use; always run a dry run first if unsure.

## 8. Customization Ideas
- Add additional kernel source files under `src/`
- Introduce a paging/long mode setup stub (if expanding beyond stub kernel)
- Extend `limine.conf` for additional menu entries
- Add `make iso` target using `xorriso` for hybrid boot

## 9. Troubleshooting
| Issue | Resolution |
|-------|------------|
| `Permission denied` during image build | Ensure you have sudo rights; the script uses loop + mount. |
| `mkfs.vfat: command not found` | Install `dosfstools`. |
| QEMU very slow | Try `ACCEL=kvm make run` (ensure `/dev/kvm` exists). |
| Breakpoint not hit | Confirm symbol `_start` exists (use `nm build/kernel.elf | grep _start`). |
| Loop device busy | Run `scripts/cleanup-loops.sh` (try `DRY_RUN=yes` first). |

## 10. License
This repository bundles Limine binaries in `limine/` which are licensed under their respective terms (see `limine/LICENSE`). Your own code here is currently unlicensed; consider adding a license file.

---
Happy hacking! Open to extending with more features (paging, memory map parsing, higher half, etc.).
