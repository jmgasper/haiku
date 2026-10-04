// SPDX-License-Identifier: MIT
// Serialize complete captured application frames after the application exits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    unsigned total = 0;
    for (unsigned phase = 0; phase < 4; phase++) {
        for (unsigned sample = 0; sample < 2; sample++) {
            char path[1024], header[80];
            int length = snprintf(path, sizeof(path), "%s/phase%u-sample%u.ppm", argv[1], phase, sample);
            if (length < 0 || length >= int(sizeof(path))) return 1;
            FILE* file = fopen(path, "rb");
            if (!file) return 1;
            unsigned width = 0, height = 0;
            char extra;
            bool valid = fgets(header, sizeof(header), file) && !strcmp(header, "P6\n")
                && fgets(header, sizeof(header), file)
                && sscanf(header, "%u %u %c", &width, &height, &extra) == 2
                && width >= 128 && width <= 512 && height >= 128 && height <= 512
                && fgets(header, sizeof(header), file) && !strcmp(header, "255\n");
            if (!valid) { fclose(file); return 1; }
            std::vector<unsigned char> rgb(width * height * 3);
            valid = fread(rgb.data(), 1, rgb.size(), file) == rgb.size() && fgetc(file) == EOF;
            if (fclose(file) != 0 || !valid) return 1;
            printf("ROCK5_APPLICATION_PIXELS_BEGIN phase=%u sample=%u width=%u height=%u format=RGB8 origin=upper-left\n",
                phase, sample, width, height);
            std::vector<char> row(width * 6 + 32);
            for (unsigned y = 0; y < height; y++) {
                int offset = snprintf(row.data(), row.size(), "row=%03u ", y);
                static const char digits[] = "0123456789abcdef";
                for (unsigned x = 0; x < width * 3; x++) {
                    unsigned byte = rgb[y * width * 3 + x];
                    row[offset++] = digits[byte >> 4];
                    row[offset++] = digits[byte & 15];
                }
                row[offset++] = '\n';
                if (fwrite(row.data(), 1, offset, stdout) != size_t(offset)) return 1;
            }
            printf("ROCK5_APPLICATION_PIXELS_END phase=%u sample=%u pixels=%u\n", phase, sample, width * height);
            total += width * height;
        }
    }
    printf("ROCK5_APPLICATION_FRAMES_PASS frames=8 pixels=%u\n", total);
    return fflush(stdout) == 0 ? 0 : 1;
}
