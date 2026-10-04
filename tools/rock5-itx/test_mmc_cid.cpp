#include "test_mmc_stubs.h"
#include "cid.inc"

int main()
{
    // Read-only Linux reference from the board, direct CID/CSD bytes shifted
    // into the standard SDHCI R2 register layout, low word first.
    const uint32 cid[4] = {0x534de21b, 0x345206ee, 0x38475446, 0x00150100};
    MMCCid mmc(cid);
    char name[8] = {};
    mmc.ProductName(name);
    assert(mmc.VendorID() == 0x15 && strcmp(name, "8GTF4R") == 0);
    assert(mmc.ProductSerial() == 0xee534de2 && mmc.ProductRevision() == 6);
    assert(mmc.ManufactureMonth() == 1 && mmc.ManufactureYear(true) == 2024);
    assert(mmc.ManufactureYear(false) == 2008);
    const uint32 csd[4] = {0xef8e4040, 0xfff6dbff, 0x320f5903, 0x00d02701};
    assert(mmc_response_bits(csd, 126, 2) == 3);
    assert(mmc_response_bits(csd, 80, 4) == 9);
    assert(mmc_response_bits(csd, 12, 2) == 0);
    const uint32 sdCid[4] = {0x56780195, 0x45321234, 0x41424344, 0x00aa5859};
    SDCid sd(sdCid);
    sd.ProductName(name);
    assert(sd.VendorID() == 0xaa && strcmp(name, "ABCDE") == 0);
    assert(sd.ProductSerial() == 0x12345678 && sd.ProductRevision() == 302);
    assert(sd.ManufactureMonth() == 5 && sd.ManufactureYear() == 2025);
    // Independent bit-by-bit oracle over all valid widths and bit positions.
    for (unsigned start = 8; start < 128; start++) {
        for (unsigned width = 1; width <= 32 && start + width <= 128; width++) {
            uint32 expected = 0;
            for (unsigned bit = 0; bit < width; bit++) {
                unsigned raw = start + bit - 8;
                expected |= ((cid[raw / 32] >> (raw % 32)) & 1) << bit;
            }
            assert(mmc_response_bits(cid, start, width) == expected);
        }
    }
    assert(mmc_response_bits(cid, 7, 1) == 0);
    assert(mmc_response_bits(cid, 127, 2) == 0);
    assert(mmc_response_bits(cid, 8, 33) == 0);
    assert(mmc_response_bits(cid, 8, 0) == 0);
    std::array<uint8, 514> extended{};
    extended.front() = 0x5a; extended.back() = 0xa5;
    // Samsung's actual 15,269,888 sectors; unsigned values beyond 2 TiB
    // of addressable bytes must also survive decoding without sign extension.
    for (uint32 sectors : {uint32(15269888), uint32(0), uint32(0x80000000), UINT32_MAX}) {
        for (unsigned i = 0; i < 4; i++)
            extended[1 + 212 + i] = sectors >> (8 * i);
        assert(mmc_ext_csd_sector_count(extended.data() + 1) == sectors);
        assert(extended.front() == 0x5a && extended.back() == 0xa5);
    }
    assert(is_mmc_card(CARD_TYPE_MMC) && is_mmc_card(CARD_TYPE_MMC_EXTENDED_CAPACITY));
    assert(!is_mmc_card(CARD_TYPE_SDHC));
    puts("MMC and SD register fixtures passed");
}
