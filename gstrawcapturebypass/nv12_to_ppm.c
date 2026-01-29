#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void nv12_to_rgb(uint8_t* nv12, uint8_t* rgb, int width, int height) {
    int frameSize = width * height;
    uint8_t *yPlane = nv12;
    uint8_t *uvPlane = nv12 + frameSize;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int yIndex = j * width + i;
            int uvIndex = (j/2) * width + (i & ~1);

            int Y = yPlane[yIndex];
            int U = uvPlane[uvIndex + 0];
            int V = uvPlane[uvIndex + 1];

            int C = Y - 16;
            int D = U - 128;
            int E = V - 128;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            if (R < 0) R = 0; if (R > 255) R = 255;
            if (G < 0) G = 0; if (G > 255) G = 255;
            if (B < 0) B = 0; if (B > 255) B = 255;

            rgb[(j*width + i)*3 + 0] = R;
            rgb[(j*width + i)*3 + 1] = G;
            rgb[(j*width + i)*3 + 2] = B;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        printf("Usage: %s <input.nv12> <output.ppm> <width> <height>\n", argv[0]);
        return 1;
    }

    const char* inFile = argv[1];
    const char* outFile = argv[2];
    int width = atoi(argv[3]);
    int height = atoi(argv[4]);

    size_t frameSize = width * height * 3 / 2;
    uint8_t* nv12 = (uint8_t*)malloc(frameSize);
    uint8_t* rgb = (uint8_t*)malloc(width * height * 3);

    FILE* fin = fopen(inFile, "rb");
    if (!fin) { printf("Failed to open input file.\n"); return 1; }
    fread(nv12, 1, frameSize, fin);
    fclose(fin);

    nv12_to_rgb(nv12, rgb, width, height);

    FILE* fout = fopen(outFile, "wb");
    fprintf(fout, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, width*height*3, fout);
    fclose(fout);

    free(nv12);
    free(rgb);

    printf("Saved as %s\n", outFile);
    return 0;
}
