/*
HOST_LOADER.C

Loads the guest image (port/macos/guest/guest.ld): a statically linked
AArch64 ELF executable, rewritten to address the window (tools/macos_sfi.py),
whose segments are copied to the window at the guest addresses it was
linked at. Its code and read-only data come first, up to its first writable
segment, and are mapped executable; the rest is mapped read-write. The image
starts with a struct halo_guest_header naming its import table, which is
filled with the host functions of the same names.
*/

#include "host.h"

#include <string.h>

struct host_guest_image host_image;

/* the ELF structures read here (macOS has no <elf.h>) */
struct elf64_header
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct elf64_program_header
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

#define ELF_CLASS_64 2
#define ELF_TYPE_EXECUTABLE 2
#define ELF_MACHINE_AARCH64 183
#define ELF_SEGMENT_LOAD 1
#define ELF_SEGMENT_WRITE 2
#define IMAGE_PAGE 0x4000ULL

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

int host_load_image(const void *file, size_t size)
{
	const struct elf64_header *elf = file;
	const struct elf64_program_header *segments;
	uint64_t low = ~0ULL, high = 0, data = ~0ULL;
	const struct halo_guest_header *header;
	uint64_t *table;
	const char *name;
	uint32_t count, index;
	int missing = 0;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, "\177ELF", 4) || elf->e_ident[4] != ELF_CLASS_64 ||
		elf->e_machine != ELF_MACHINE_AARCH64 || elf->e_type != ELF_TYPE_EXECUTABLE ||
		elf->e_phoff + (uint64_t)elf->e_phnum * sizeof(*segments) > size)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	segments = (const struct elf64_program_header *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		const struct elf64_program_header *segment = &segments[index];

		if (segment->p_type != ELF_SEGMENT_LOAD)
			continue;
		if (segment->p_vaddr < low)
			low = segment->p_vaddr;
		if (segment->p_vaddr + segment->p_memsz > high)
			high = segment->p_vaddr + segment->p_memsz;
		if ((segment->p_flags & ELF_SEGMENT_WRITE) && segment->p_vaddr < data)
			data = segment->p_vaddr;
		if (segment->p_offset + segment->p_filesz > size)
		{
			host_logf(HOST_LOG_ERROR, "the guest image is cut short");
			return -1;
		}
	}
	low &= ~(IMAGE_PAGE - 1);
	high = (high + IMAGE_PAGE - 1) & ~(IMAGE_PAGE - 1);
	data &= ~(IMAGE_PAGE - 1);
	if (low >= high || high > HOST_WINDOW_SIZE || data <= low || data >= high)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx (data at %llx)", (unsigned long long)low,
			(unsigned long long)high, (unsigned long long)data);
		return -1;
	}
	if (host_memory_map_image((uint32_t)low, (uint32_t)data, (uint32_t)data, (uint32_t)high) != 0)
		return -1;
	for (index = 0; index < elf->e_phnum; index++)
	{
		const struct elf64_program_header *segment = &segments[index];

		if (segment->p_type != ELF_SEGMENT_LOAD)
			continue;
		memcpy(HOST_POINTER(void *, segment->p_vaddr), (const char *)file + segment->p_offset, segment->p_filesz);
	}

	header = HOST_POINTER(const struct halo_guest_header *, low);
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		return -1;
	}
	host_image.header = header;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;

	table = HOST_POINTER(uint64_t *, header->import_table);
	name = HOST_POINTER(const char *, header->import_names);
	count = *HOST_POINTER(const uint32_t *, header->import_count);
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_memory_seal_code();
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx (code to %08llx), %u imports (%d unavailable)",
		(unsigned long long)low, (unsigned long long)high, (unsigned long long)data, count, missing);
	return 0;
}
