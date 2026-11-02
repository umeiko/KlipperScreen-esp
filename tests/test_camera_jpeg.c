#include "camera_jpeg.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void check_yellow(const camera_frame_t *frame, unsigned x, unsigned y)
{
    const uint16_t *pixels = (const uint16_t *)frame->dsc.data;
    uint16_t px = pixels[y * frame->dsc.header.w + x];
    assert((px & 0xf800) == 0xf800);
    assert((px & 0x07e0) == 0x07e0);
    assert((px & 0x001f) == 0);
}

int main(void)
{
    FILE *f = fopen("tests/camera_pattern.jpg", "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t *jpeg = malloc((size_t)size);
    assert(jpeg && fread(jpeg, 1, (size_t)size, f) == (size_t)size);
    fclose(f);

    camera_frame_t *normal = NULL, *rotated = NULL, *invalid = NULL;
    assert(camera_jpeg_decode(jpeg, (size_t)size, 32, 32, false, false, 0, &normal));
    assert(normal && normal->dsc.header.w == 32 && normal->dsc.header.h == 24);
    check_yellow(normal, 4, 4);
    assert(camera_jpeg_decode(jpeg, (size_t)size, 32, 32, false, false, 90, &rotated));
    assert(rotated && rotated->dsc.header.w == 24 && rotated->dsc.header.h == 32);
    check_yellow(rotated, 19, 4);
    assert(!camera_jpeg_decode(jpeg, 4, 32, 32, false, false, 0, &invalid));
    camera_frame_free(normal);
    camera_frame_free(rotated);
    free(jpeg);

    f = fopen("tests/camera_1080p.jpg", "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    jpeg = malloc((size_t)size);
    assert(jpeg && fread(jpeg, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    normal = rotated = NULL;
    assert(camera_jpeg_decode(jpeg, (size_t)size, 320, 240, false, false, 0, &normal));
    assert(normal && normal->dsc.header.w == 320 && normal->dsc.header.h == 180);
    check_yellow(normal, 4, 4);
    assert(camera_jpeg_decode(jpeg, (size_t)size, 320, 240, false, false, 90, &rotated));
    assert(rotated && rotated->dsc.header.w == 135 && rotated->dsc.header.h == 240);
    camera_frame_free(normal);
    camera_frame_free(rotated);
    free(jpeg);
    puts("PASS: JPEG BGR-to-RGB565, 90-degree rotation and 1080p DCT downscaling");
    return 0;
}
