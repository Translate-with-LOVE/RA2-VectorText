// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelPlane.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>

int main()
{
    constexpr int width = 4, height = 3, sourcePitch = 6, outputPitch = 5;
    constexpr uint32_t sentinel = 0x12345678;
    vt::PixelPlane plane(width, height, {});
    std::array<unsigned short, sourcePitch * height> source{};
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<unsigned short>(i * 1703);
    // Populate a tile so empty/reversed rect tests also cover tile iteration.
    plane.Set(1, 1, {65535, 0, 0, 65535});
    using Operation = void (vt::PixelPlane::*)(const unsigned short*, int, uint32_t*, int, vt::PixelRect) const;
    const Operation operations[] = {&vt::PixelPlane::BackgroundRect, &vt::PixelPlane::CompositeRect};
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    for (const auto operation : operations)
    {
        std::array<uint32_t, outputPitch * height + 2> output;
        const auto noWrite = [&](const unsigned short *input, int inputPitch, uint32_t *dest,
                                 int destPitch, vt::PixelRect rect) {
            output.fill(sentinel);
            (plane.*operation)(input, inputPitch, dest, destPitch, rect);
            check(std::all_of(output.begin(), output.end(), [=](auto p) { return p == sentinel; }),
                  "invalid blit leaves output untouched");
        };
        const vt::PixelRect full{0, 0, width, height};
        noWrite(nullptr, sourcePitch, output.data() + 1, outputPitch, full);
        noWrite(source.data(), sourcePitch, nullptr, outputPitch, full);
        for (int pitch : {INT_MIN, -1, 0, width - 1})
        {
            noWrite(source.data(), pitch, output.data() + 1, outputPitch, full);
            noWrite(source.data(), sourcePitch, output.data() + 1, pitch, full);
        }
        noWrite(source.data(), INT_MAX, output.data() + 1, outputPitch, full);
        noWrite(source.data(), sourcePitch, output.data() + 1, INT_MAX, full);
        for (auto rect : {vt::PixelRect{0, 0, 0, 0}, {2, 1, 1, 2}, {0, 2, 4, 1},
                          {10, 10, 20, 20}, {INT_MAX, INT_MAX, INT_MIN, INT_MIN},
                          {INT_MIN, INT_MIN, -1, -1}})
            noWrite(source.data(), sourcePitch, output.data() + 1, outputPitch, rect);
        output.fill(sentinel);
        (plane.*operation)(source.data(), sourcePitch, output.data() + 1, outputPitch,
                           {INT_MIN, INT_MIN, INT_MAX, INT_MAX});
        check(output.front() == sentinel && output.back() == sentinel, "full clip preserves guards");
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < outputPitch; ++x)
                check(output[1 + y * outputPitch + x] == (x >= width ? sentinel :
                    operation == &vt::PixelPlane::CompositeRect ? plane.Composite(x, y, source[y * sourcePitch + x]) :
                    vt::PixelPlane::Expand565(source[y * sourcePitch + x])), "padded full blit agrees with scalar reference");
        output.fill(sentinel);
        (plane.*operation)(source.data(), sourcePitch, output.data() + 1, outputPitch, {-5, 1, 3, 9});
        for (size_t i = 0; i < output.size(); ++i)
        {
            uint32_t expected = sentinel;
            if (i >= 1 && i < 1 + outputPitch * 2)
            {
                const int x = static_cast<int>((i - 1) % outputPitch), y = static_cast<int>((i - 1) / outputPitch) + 1;
                if (x < 3) expected = operation == &vt::PixelPlane::CompositeRect ?
                    plane.Composite(x, y, source[y * sourcePitch + x]) : vt::PixelPlane::Expand565(source[y * sourcePitch + x]);
            }
            check(output[i] == expected, "partial clipping retains clipped output origin and guards");
        }
    }
    std::printf("pixel contracts: %d failures\n", failures);
    return failures ? 1 : 0;
}
