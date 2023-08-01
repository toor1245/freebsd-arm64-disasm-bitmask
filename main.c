#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

static bool
arm64_is_bit_set(uint64_t value, uint32_t bit)
{
	return ((value >> bit) & 0x1);
}

/*
 * Returns the highest set bit of `value`, search performs from
 * most significant bit. If highest set bit is not found, we return -1.
 */
static int
arm64_highest_set_bit(uint64_t value)
{
	for (int i = sizeof(uint64_t) * CHAR_BIT - 1; i >= 0; i--) {
		if (arm64_is_bit_set(value, i))
			return (i);
	}

	return (-1);
}

/*
 * Creates a 64 bit value with a specified number of ones starting from lsb.
 *
 * Example:
 * 	`length` = 7
 * 	`result` = 0b1111111
 */
static uint64_t
arm64_ones(uint32_t length)
{
	return ((1ULL << length) - 1);
}

/* Replicates `value` bits `esize` times with a fixed size `bit_count`.
 *
 * Example:
 * 	`value`  = 0b10010011, `esize` = 8, `bit_count` = 32
 * 	`result` = 0b10010011_10010011_10010011_10010011
 */
static uint64_t
arm64_replicate(uint64_t value, uint32_t esize, int bit_count)
{
	uint64_t result, set_bits;

	result = value;

	for (set_bits = esize; set_bits < bit_count; set_bits += esize) {
		value <<= esize;
		result |= value;
	}

	return (result);
}

/*
 * Performs circular shift to the right, shifts all bits of a binary number
 * `value` by `shift_count`, the least significant bit is pushed out.
 *
 * Example:
 * 	`value`  = 0b0001_1101_0110_1011, `shift_count` = 2, `width` = 16
 *	`result` = 0b1100_0111_0101_1010
 */
static uint64_t
arm64_ror(uint64_t value, uint32_t shift_count, uint32_t width)
{
	uint64_t result, right_shift, left_shift;

	right_shift = shift_count;
	left_shift = width - shift_count;
	result = value >> right_shift;
	result |= value << left_shift;

	/*
	 * Ignores redundant bits that we can get in result after left shift
	 */
	if (width < 64)
		result &= arm64_ones(width);

	return (result);
}

/*
 * Returns true if bitmask is decoded successfully.
 * According to Arm64 documentation we must return UNDEFINED
 * in case of invalid parameters, thus we use false as UNDEFINED
 * and on high flow of codebase we must print undefined.
 */
static bool
arm64_disasm_bit_masks(uint32_t n, uint32_t imms, uint32_t immr,
    bool logical_imm, uint64_t *wmask)
{
	uint64_t welem;
	uint32_t levels, s, r;
	int length, esize;

	/*
	 * Finds the highest set bit of immN:NOT(imms).
	 * Total bit count of immN(1) and imms(6) is 7,
	 * thus we start from 6 index.
	 */
	length = arm64_highest_set_bit((n << 6) | (~imms & 0x3F));

	if (length < 1)
		return (false);

	levels = arm64_ones(length);

	/*
	 * For logical immediates an all-ones value of S is reserved
	 * since it would generate a useless all-ones result (many times)
	 */
	if (logical_imm && (imms & levels) == levels)
		return (false);

	s = imms & levels;
	r = immr & levels;

	esize = 1 << length;
	welem = arm64_ones(s + 1);
	*wmask = arm64_ror(welem, r, esize);
	*wmask = arm64_replicate(*wmask, esize, sizeof(uint64_t) * CHAR_BIT);

	return (true);
}


/*
 * Returns true if bitmask immediate would generate an immediate value that
 * also could be represented by a single MOVZ, MOVN or MOV (wide immediate)
 * instruction. Also, this function determines should we use
 * MOV (bitmask immediate) alias or ORR (immediate).
 *
 * Example:
 * 	  `sf` = 1, `immn` = 1, `imms` = 0b011100, `immr` = 0b000011
 * 	   First of all, we define `width`, since `sf` is 1 `width` will be 64.
 * 	   Next step is to combine `immn` and `imms` to check element size
 * 	   on total immediate size. So, immN:imms(7 bit) = 0b1011100.
 * 	   and immN:imms matches to 0b1xxxxxx pattern. We skip checks
 * 	   "imms < 16", imms greater than 16 and `imms` is not greater than
 * 	   `width` - 15, thus move wide is not preferred and immediate
 * 	   value e000000003ffffff can be used for MOV (bitmask immediate)
 * 	   if Rn register is 31.
 */
static bool
arm64_move_wide_preferred(int sf, uint32_t immn, uint32_t imms,
    uint32_t immr)
{
	int width;

	width = sf == 1 ? 64 : 32;

	/*
	 * Element size must equal total immediate size.
	 * - for 64 bit immN:imms == '0b1xxxxxx'
	 * - for 32 bit immN:imms == '0b00xxxxx'
	 * Since, we know that immN:imms is 7 bit and patterns only take into
	 * account msb bits, no need to make bit pattern and masks.
	 * Hence, we can check only certain bits.
	 */
	if (sf == 1 && immn != 1)
		return (false);
	if (sf == 0 && (immn != 0 || arm64_is_bit_set(imms, 6)))
		return (false);

	/* For MOVZ, imms must contain no more than 16 ones */
	if (imms < 16)
		/* Ones must not span halfword boundary when rotated */
		return (-immr % 16 <= 15 - imms);

	/* For MOVZ, imms must contain no more than 16 zeros */
	if (imms >= width - 15)
		/* Zeros must not span halfword boundary when rotated */
		return (immr % 16 <= imms - width - 15);

	return (false);
}

int main()
{
	FILE *file = NULL;
	char *line = NULL;
	uint64_t wmask = 0;
    	size_t len = 0;
    	ssize_t read = 0;
	uint64_t immn = 0;
       	uint64_t immr = 0;
	uint64_t imms = 0;
	uint64_t imm = 0;
	char *subline;
	bool is_decoded = false;

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
			if (i == 5)
				immn = atoi(subline + 2);
			if (i == 6)
				immr = strtoul(subline + 5, NULL, 2);
			if (i == 7)
				imms = strtoul(subline + 5, NULL, 2);
	  	}
		printf("imm: 0x%lx\timmn: %lu immr: %lu imms: %lu",
		    imm, immn, immr, imms);
		is_decoded = arm64_disasm_bit_masks(immn, imms, immr, true, &wmask);
		printf(", decoded: %d, arm64_disasm_bitmask: %lx, imm == wmask: %d\n",
		    is_decoded, wmask, imm == wmask);
		if (imm != wmask) {
			printf("ERROR: decoded invalid");
			return 0;
		}
    	}

    	fclose(file);
    	if (line)
        	free(line);

	return 0;
}
