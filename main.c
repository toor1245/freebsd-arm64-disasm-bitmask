#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static bool
arm64_is_bit_set(uint64_t reg, uint32_t bit)
{
    return ((reg >> bit) & 0x1);
}

static uint64_t
arm64_ones(uint32_t length)
{
    return ((1ULL << length) - 1);
}

static int
arm64_highest_set_bit(uint64_t value, int msb_start)
{
    for (int i = msb_start; i >= 0; i--) {
        if (arm64_is_bit_set(value, i)) {
            return i;
        }
    }
    return -1;
}

uint64_t
arm64_ror(uint64_t value, uint32_t sh, uint32_t width)
{
    uint64_t result, rsh, lsh;

    rsh = sh;
    lsh = width - rsh;
    result = value >> rsh;
    result |= value << lsh;

    if (width < 64) {
        // mask off any extra bits that we got from the left shift
        result &= arm64_ones(width);
    }
    return result;
}

uint64_t
arm64_replicate(uint64_t value, uint32_t width, int imm_width)
{
    uint64_t result;
    uint64_t set_bits;

    result = value;
    for (set_bits = value; set_bits < imm_width; set_bits += width) {
        value <<= width;
        result |= value;
    }
    return result;
}

static bool
arm64_disasm_bit_masks(uint8_t n, uint32_t imms, uint32_t immr, bool imm, uint64_t *wmask)
{
    uint64_t welem;
    uint32_t levels, s, r;
    int width, length, esize;

    width = n == 1 ? 64 : 32;
    length = arm64_highest_set_bit((n << 6) | (~imms & 0x3F), 6);

    if (length < 1)
        return false;

    levels = arm64_ones(length);

    /*
     * According to Arm A64 Instruction Set Architecture:
     * for logical immediates an all-ones value of S is reserved
     * since it would generate a useless all-ones result (many times)
     */
    if (imm && (imms & levels) == levels)
        return false;

    s = imms & levels;
    r = immr & levels;

    esize = 1 << length;
    welem = arm64_ones(s + 1);
    *wmask = arm64_ror(welem, r, esize);
    *wmask = arm64_replicate(*wmask, esize, width);
    return true;
}


int main()
{
	FILE *file = NULL;
	char *line = NULL;
    	size_t len = 0;
    	ssize_t read = 0;
	int immn = 0;
	unsigned long immr = 0;
	unsigned long imms = 0;
	unsigned long long imm = 0;
	char *subline;

	file = fopen("./all_possible_bitmask_imm.txt", "r");
	if (file == NULL) {
		printf("fopen(): failed.");
		return 1;
	}

	while ((read = getline(&line, &len, file)) != -1) {
		subline = strtok(line, " ");
		imm = strtoull(subline, NULL, 16);

		for (int i = 1; subline != NULL; ++i) {
	    		subline = strtok(NULL, " ");
			if (i == 5) {
				immn = atoi(subline + 2);
			}
			if (i == 6) {
				immr = strtoul(subline + 5, NULL, 2);
			}
			if (i == 7) {
				imms = strtoul(subline + 5, NULL, 2);
			}
	  	}
		printf("imm: 0x%llx\timmn: %ld immr: %ld imms: %ld\n",
		    imm, immn, immr, imms);
    	}

    	fclose(file);
    	if (line)
        	free(line);

	return 0;
}
