import asyncio
import telnetlib3
import re
import time

HOST = '127.0.0.1'
PORT = 8086
RETRY_DELAY = 2  # segundos entre intentos de conexión

def format_memory_block(block_str):
    bytes_list = block_str.split()
    lines = []
    for i in range(0, len(bytes_list), 8):
        start = i
        end = i + 8 - 1
        line = f"bytes {start:02}-{end:02}: " + " ".join(bytes_list[i:i+8])
        lines.append(line)
    return lines

def parse_line(line):
    kmalloc_match = re.match(
        r'kmalloc\((\d+)\) reservado: inicio=(0x[0-9a-fA-F]+) fin=(0x[0-9a-fA-F]+) tamaño=(\d+)',
        line
    )
    if kmalloc_match:
        size, start, end, real_size = kmalloc_match.groups()
        return (
            f"kmalloc({size}) reservado:\n"
            f"    inicio : {start.lower()}\n"
            f"    fin    : {end.lower()}\n"
            f"    tamaño : {real_size} bytes"
        )

    mem_match = re.match(r'Contenido.*?bytes.*?: \[.*?\] = ([0-9a-fx ]+)', line)
    if mem_match:
        return "\n".join(format_memory_block(mem_match.group(1)))

    return line.strip()

async def connect_and_listen():
    while True:
        try:
            print(f"Intentando conectarse a {HOST}:{PORT}...")
            reader, writer = await telnetlib3.open_connection(HOST, PORT)
            print("Conectado al kernel. Mostrando salida en directo:\n")
            
            while True:
                line = await reader.readline()
                if not line:
                    # Conexión cerrada por el servidor
                    print("Conexión cerrada por el kernel. Reintentando...")
                    break
                formatted = parse_line(line)
                if formatted:
                    print(formatted)
        except (ConnectionRefusedError, OSError):
            # No hay servidor escuchando aún
            print(f"No hay servidor en {HOST}:{PORT}, reintentando en {RETRY_DELAY} segundos...")
            await asyncio.sleep(RETRY_DELAY)
        except EOFError:
            print("EOF recibido, esperando nueva conexión...")
            await asyncio.sleep(RETRY_DELAY)

asyncio.run(connect_and_listen())

