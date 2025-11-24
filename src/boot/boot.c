#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/drivers/serial/serial.h>
#include <krnl/devices/devices.h>
#include <krnl/libraries/std/string.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/debug/debug.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/std/stdio.h>

void boot_startup() {
    // ===== INICIALIZACIÓN DEL SISTEMA =====
    
    // Inicializar el bootloader para obtener información del arranque
    init_bootloader();
    
    // Inicializar soporte para instrucciones SIMD (MMX, SSE, AVX, etc.)
    arch_init_simd();
    
    // Inicializar el subsistema de dispositivos del kernel
    devices_init();
    
    // Inicializar comunicación serie Plug-and-Play para depuración temprana
    serial_init_pnp();
    
    // Inicializar el sistema de depuración con nivel de verbosidad 3
    debug_init(3, 0);

    // ===== MENSAJES DE INICIO =====
    
    // Mostrar mensaje de arranque exitoso
    kprintf("ASOC KERNEL BOOTED SUCCESSFULLY!\n");
    
    // Mostrar información del bootloader utilizado
    kprintf("Using bootloader: %s version: %s\n", get_bootloader_name(), get_bootloader_version());

    // ===== DEMOSTRACIÓN DE KMALLOC - RESERVA DE MEMORIA =====
    
    /* Definir el tamaño de memoria a reservar para la prueba */
    const uint64_t alloc_size = 1028;

    // Medir la memoria utilizada antes de la reserva
    uint64_t used_before = kmalloc_used();
    
    // Reservar bloque de memoria
    void *p = kmalloc(alloc_size);
    
    // Medir la memoria utilizada después de la reserva
    uint64_t used_after = kmalloc_used();

    // Verificar si la reserva fue exitosa
    if (p) {
        // Calcular la memoria realmente reservada (puede incluir overhead)
        uint64_t reserved = used_after - used_before;
        
        // Calcular direcciones de inicio y fin del bloque
        void *block_start = p;
        void *block_end = (void *)((unsigned char*)p + alloc_size - 1);
        
        // ===== CABECERA DE LA DEMOSTRACIÓN =====
        kprintf("=== KMALLOC DEMO ===\n");
        
        // Mostrar información básica de la reserva
        kprintf("kmalloc(%u) reservado:\n", (unsigned)alloc_size);
        kprintf("  inicio : 0x%016llx\n", (unsigned long long)(uintptr_t)block_start);
        kprintf("  fin    : 0x%016llx\n", (unsigned long long)(uintptr_t)block_end);
        kprintf("  resta  : %llu bytes\n", (unsigned long long)(uintptr_t)block_end - (unsigned long long)(uintptr_t)block_start + 1);
        kprintf("  tamaño : %llu bytes\n", reserved);

        // ===== MOSTRAR CONTENIDO INICIAL DE LA MEMORIA =====
        /* Mostrar los primeros 16 bytes del bloque ANTES de llenarlo
         * Esto nos permite ver el contenido inicial (basura/zeros) devuelto por kmalloc
         */
        kprintf("  Contenido inicial (primeros 16 bytes):\n");
        for (int base = 0; base < 16; base += 8) {
            char buf[128];
            int pos = 0;
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "  bytes %02d-%02d: ", base, base+7);
            for (int i = 0; i < 8; i++) {
                pos += snprintf_(buf + pos, sizeof(buf) - pos, "%02x ", ((unsigned char*)p)[base + i]);
            }
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "\n");
            kprintf("%s", buf);
        }

        // ===== ESCRIBIR PATRÓN EN LA MEMORIA =====
        /* Rellenar toda la memoria reservada con el patrón 0xAA
         * Esto demuestra que podemos escribir en la memoria reservada
         */
        memset(p, 0xAA, alloc_size);

        // ===== VERIFICAR ESCRITURA - PRIMEROS BYTES =====
        /* Mostrar los primeros 16 bytes después del memset
         * Deberían mostrar el patrón 0xAA confirmando la escritura exitosa
         */
        kprintf("  Después de memset(0xAA) - primeros 16 bytes:\n");
        for (int base = 0; base < 16; base += 8) {
            char buf[128];
            int pos = 0;
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "  bytes %02d-%02d: ", base, base+7);
            for (int i = 0; i < 8; i++) {
                pos += snprintf_(buf + pos, sizeof(buf) - pos, "%02x ", ((unsigned char*)p)[base + i]);
            }
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "\n");
            kprintf("%s", buf);
        }

        // ===== VERIFICAR ESCRITURA - ÚLTIMOS BYTES =====
        /* Mostrar los últimos 16 bytes del bloque
         * Confirma que todo el rango solicitado fue escrito correctamente
         */
        kprintf("  Después de memset(0xAA) - últimos 16 bytes:\n");
        for (int block = 0; block < 2; block++) {
            int start = alloc_size - 16 + block*8;
            char buf[128];
            int pos = 0;
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "  bytes %04d-%04d: ", start, start+7);
            for (int i = 0; i < 8; i++) {
                pos += snprintf_(buf + pos, sizeof(buf) - pos, "%02x ", ((unsigned char*)p)[start + i]);
            }
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "\n");
            kprintf("%s", buf);
        }
    } else {
        // Manejar fallo en la reserva de memoria
        kprintf("kmalloc(%u) falló\n", (unsigned)alloc_size);
    }

    // ===== DEMOSTRACIÓN DE KFREE Y REUTILIZACIÓN =====
    kprintf("--- DEMO kfree/reuse ---\n");
    
    // Reservar un bloque pequeño adicional
    const uint64_t small_size = 128;
    void *b = kmalloc(small_size);
    if (b) {
        char buf[128];
        int pos = 0;
        pos += snprintf_(buf + pos, sizeof(buf) - pos, "kmalloc(%u) -> %p  ", (unsigned)small_size, b);
        
        // Escribir patrón diferente en el bloque pequeño
        memset(b, 0x55, small_size);
        pos += snprintf_(buf + pos, sizeof(buf) - pos, "Primer byte de b=0x%02x\n", ((unsigned char*)b)[0]);
        kprintf("%s", buf);
    }

    // ===== LIBERAR MEMORIA Y REUTILIZAR =====
    
    // Liberar el bloque grande reservado anteriormente
    kfree(p);
    kprintf("Se llamó kfree(p) sobre %p\n", p);

    // Intentar reservar un bloque de tamaño medio que debería reutilizar el espacio liberado
    const uint64_t reuse_size = 2000;
    void *q = kmalloc(reuse_size);
    if (q) {
        kprintf("kmalloc(%u) -> %p (tras free)\n", (unsigned)reuse_size, q);
        
        // Mostrar contenido del bloque recién asignado (puede contener datos previos)
        {
            char buf[256];
            int pos = 0;
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "Primeros 8 bytes de q antes de memset: ");
            for (int i = 0; i < 8; i++) 
                pos += snprintf_(buf + pos, sizeof(buf) - pos, "%02x ", ((unsigned char*)q)[i]);
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "\n");
            kprintf("%s", buf);
        }
        
        // Escribir nuevo patrón en el bloque reutilizado
        memset(q, 0xCC, reuse_size);
        
        // Verificar que se escribió correctamente
        {
            char buf[256];
            int pos = 0;
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "Primeros 8 bytes de q después de memset: ");
            for (int i = 0; i < 8; i++) 
                pos += snprintf_(buf + pos, sizeof(buf) - pos, "%02x ", ((unsigned char*)q)[i]);
            pos += snprintf_(buf + pos, sizeof(buf) - pos, "\n");
            kprintf("%s", buf);
        }
    } else {
        kprintf("kmalloc(%u) (tras free) falló\n", (unsigned)reuse_size);
    }

    // ===== FINALIZACIÓN =====
    
    // Bucle infinito - el kernel permanece ejecutándose
    while (1);
}