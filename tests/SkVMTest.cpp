/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkColorPriv.h"
#include "include/private/SkColorData.h"
#include "src/core/SkCpu.h"
#include "src/core/SkMSAN.h"
#include "src/core/SkVM.h"
#include "tests/Test.h"
#include "tools/Resources.h"
#include "tools/SkVMBuilders.h"

using Fmt = SrcoverBuilder_F32::Fmt;
const char* fmt_name(Fmt fmt) {
    switch (fmt) {
        case Fmt::A8:        return "A8";
        case Fmt::G8:        return "G8";
        case Fmt::RGBA_8888: return "RGBA_8888";
    }
    return "";
}

static void dump(skvm::Builder& builder, SkWStream* o) {
    skvm::Program program = builder.done();
    builder.dump(o);
    o->writeText("\n");
    program.dump(o);
    o->writeText("\n");
}

// TODO: I'd like this to go away and have every test in here run both JIT and interpreter.
template <typename Fn>
static void test_interpreter_only(skiatest::Reporter* r, skvm::Program&& program, Fn&& test) {
    REPORTER_ASSERT(r, !program.hasJIT());
    test((const skvm::Program&) program);
}

template <typename Fn>
static void test_jit_and_interpreter(skiatest::Reporter* r, skvm::Program&& program, Fn&& test) {
    static const bool can_jit = []{
        // This is about the simplest program we can write, setting an int buffer to a constant.
        // If this can't JIT, the platform does not support JITing.
        skvm::Builder b;
        b.store32(b.varying<int>(), b.splat(42));
        skvm::Program p = b.done();
        return p.hasJIT();
    }();

    if (can_jit) {
        REPORTER_ASSERT(r, program.hasJIT());
        test((const skvm::Program&) program);
        program.dropJIT();
    }
    test_interpreter_only(r, std::move(program), std::move(test));
}


DEF_TEST(SkVM, r) {
    SkDynamicMemoryWStream buf;

    // Write all combinations of SrcoverBuilder_F32
    for (int s = 0; s < 3; s++)
    for (int d = 0; d < 3; d++) {
        auto srcFmt = (Fmt)s,
             dstFmt = (Fmt)d;
        SrcoverBuilder_F32 builder{srcFmt, dstFmt};

        buf.writeText(fmt_name(srcFmt));
        buf.writeText(" over ");
        buf.writeText(fmt_name(dstFmt));
        buf.writeText("\n");
        dump(builder, &buf);
    }

    // Write the I32 Srcovers also.
    {
        SrcoverBuilder_I32_Naive builder;
        buf.writeText("I32 (Naive) 8888 over 8888\n");
        dump(builder, &buf);
    }
    {
        SrcoverBuilder_I32 builder;
        buf.writeText("I32 8888 over 8888\n");
        dump(builder, &buf);
    }
    {
        SrcoverBuilder_I32_SWAR builder;
        buf.writeText("I32 (SWAR) 8888 over 8888\n");
        dump(builder, &buf);
    }

    {
        // Demonstrate the value of program reordering.
        skvm::Builder b;
        skvm::Arg sp = b.varying<int>(),
                  dp = b.varying<int>();

        skvm::I32 byte = b.splat(0xff);

        skvm::I32 src = b.load32(sp),
                  sr  = b.extract(src,  0, byte),
                  sg  = b.extract(src,  8, byte),
                  sb  = b.extract(src, 16, byte),
                  sa  = b.extract(src, 24, byte);

        skvm::I32 dst = b.load32(dp),
                  dr  = b.extract(dst,  0, byte),
                  dg  = b.extract(dst,  8, byte),
                  db  = b.extract(dst, 16, byte),
                  da  = b.extract(dst, 24, byte);

        skvm::I32 R = b.add(sr, dr),
                  G = b.add(sg, dg),
                  B = b.add(sb, db),
                  A = b.add(sa, da);

        skvm::I32 rg = b.pack(R, G, 8),
                  ba = b.pack(B, A, 8),
                  rgba = b.pack(rg, ba, 16);

        b.store32(dp, rgba);

        dump(b, &buf);
    }

    // Our checked in dump expectations assume we have FMA support.
    const bool fma_supported =
    #if defined(SK_CPU_X86)
        SkCpu::Supports(SkCpu::HSW);
    #elif defined(SK_CPU_ARM64)
        true;
    #else
        false;
    #endif
    if (fma_supported) {
        sk_sp<SkData> blob = buf.detachAsData();
        {

            sk_sp<SkData> expected = GetResourceAsData("SkVMTest.expected");
            REPORTER_ASSERT(r, expected, "Couldn't load SkVMTest.expected.");
            if (expected) {
                if (blob->size() != expected->size()
                        || 0 != memcmp(blob->data(), expected->data(), blob->size())) {

                    ERRORF(r, "SkVMTest expected\n%.*s\nbut got\n%.*s\n",
                           expected->size(), expected->data(),
                           blob->size(), blob->data());
                }

                SkFILEWStream out(GetResourcePath("SkVMTest.expected").c_str());
                if (out.isValid()) {
                    out.write(blob->data(), blob->size());
                }
            }
        }
    }

    auto test_8888 = [&](skvm::Program&& program) {
        uint32_t src[9];
        uint32_t dst[SK_ARRAY_COUNT(src)];

        test_jit_and_interpreter(r, std::move(program), [&](const skvm::Program& program) {
            for (int i = 0; i < (int)SK_ARRAY_COUNT(src); i++) {
                src[i] = 0xbb007733;
                dst[i] = 0xffaaccee;
            }

            SkPMColor expected = SkPMSrcOver(src[0], dst[0]);  // 0xff2dad73

            program.eval((int)SK_ARRAY_COUNT(src), src, dst);

            // dst is probably 0xff2dad72.
            for (auto got : dst) {
                auto want = expected;
                for (int i = 0; i < 4; i++) {
                    uint8_t d = got  & 0xff,
                            w = want & 0xff;
                    if (abs(d-w) >= 2) {
                        SkDebugf("d %02x, w %02x\n", d,w);
                    }
                    REPORTER_ASSERT(r, abs(d-w) < 2);
                    got  >>= 8;
                    want >>= 8;
                }
            }
        });
    };

    test_8888(SrcoverBuilder_F32{Fmt::RGBA_8888, Fmt::RGBA_8888}.done("srcover_f32"));
    test_8888(SrcoverBuilder_I32_Naive{}.done("srcover_i32_naive"));
    test_8888(SrcoverBuilder_I32{}.done("srcover_i32"));
    test_8888(SrcoverBuilder_I32_SWAR{}.done("srcover_i32_SWAR"));

    test_jit_and_interpreter(r, SrcoverBuilder_F32{Fmt::RGBA_8888, Fmt::G8}.done(),
                             [&](const skvm::Program& program) {
        uint32_t src[9];
        uint8_t  dst[SK_ARRAY_COUNT(src)];

        for (int i = 0; i < (int)SK_ARRAY_COUNT(src); i++) {
            src[i] = 0xbb007733;
            dst[i] = 0x42;
        }

        SkPMColor over = SkPMSrcOver(SkPackARGB32(0xbb, 0x33, 0x77, 0x00),
                                     0xff424242);

        uint8_t want = SkComputeLuminance(SkGetPackedR32(over),
                                          SkGetPackedG32(over),
                                          SkGetPackedB32(over));
        program.eval((int)SK_ARRAY_COUNT(src), src, dst);

        for (auto got : dst) {
            REPORTER_ASSERT(r, abs(got-want) < 3);
        }
    });

    test_jit_and_interpreter(r, SrcoverBuilder_F32{Fmt::A8, Fmt::A8}.done(),
                             [&](const skvm::Program& program) {
        uint8_t src[256],
                dst[256];
        for (int i = 0; i < 256; i++) {
            src[i] = 255 - i;
            dst[i] = i;
        }

        program.eval(256, src, dst);

        for (int i = 0; i < 256; i++) {
            uint8_t want = SkGetPackedA32(SkPMSrcOver(SkPackARGB32(src[i], 0,0,0),
                                                      SkPackARGB32(     i, 0,0,0)));
            REPORTER_ASSERT(r, abs(dst[i]-want) < 2);
        }
    });
}

DEF_TEST(SkVM_eliminate_dead_code, r) {
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<int>();
        skvm::I32 l = b.load32(arg);
        skvm::I32 a = b.add(l, l);
        b.add(a, b.splat(7));
    }

    std::vector<skvm::Instruction> program = b.program();
    REPORTER_ASSERT(r, program.size() == 4);

    program = skvm::eliminate_dead_code(program);
    REPORTER_ASSERT(r, program.size() == 0);
}

DEF_TEST(SkVM_Usage, r) {
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<int>(),
                  buf = b.varying<int>();
        skvm::I32 l = b.load32(arg);
        skvm::I32 a = b.add(l, l);
        skvm::I32 s = b.add(a, b.splat(7));
        b.store32(buf, s);
    }

    skvm::Usage usage{b.program()};
    REPORTER_ASSERT(r, b.program()[0].op == skvm::Op::load32);
    REPORTER_ASSERT(r, usage[0].size() == 2);
    REPORTER_ASSERT(r, b.program()[1].op == skvm::Op::add_i32);
    REPORTER_ASSERT(r, usage[1].size() == 1);
    REPORTER_ASSERT(r, b.program()[2].op == skvm::Op::splat);
    REPORTER_ASSERT(r, usage[2].size() == 1);
    REPORTER_ASSERT(r, b.program()[3].op == skvm::Op::add_i32);
    REPORTER_ASSERT(r, usage[3].size() == 1);
}

DEF_TEST(SkVM_Pointless, r) {
    // Let's build a program with no memory arguments.
    // It should all be pegged as dead code, but we should be able to "run" it.
    skvm::Builder b;
    {
        b.add(b.splat(5.0f),
              b.splat(4.0f));
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        for (int N = 0; N < 64; N++) {
            program.eval(N);
        }
    });

    for (const skvm::OptimizedInstruction& inst : b.optimize()) {
        REPORTER_ASSERT(r, inst.death == 0 && inst.can_hoist == true);
    }
}

#if defined(SKVM_LLVM)
DEF_TEST(SkVM_LLVM_memset, r) {
    skvm::Builder b;
    b.store32(b.varying<int>(), b.splat(42));

    skvm::Program p = b.done();
    REPORTER_ASSERT(r, p.hasJIT());

    int buf[18];
    buf[17] = 47;

    p.eval(17, buf);
    for (int i = 0; i < 17; i++) {
        REPORTER_ASSERT(r, buf[i] == 42);
    }
    REPORTER_ASSERT(r, buf[17] == 47);
}

DEF_TEST(SkVM_LLVM_memcpy, r) {
    skvm::Builder b;
    {
        auto src = b.varying<int>(),
             dst = b.varying<int>();
        b.store32(dst, b.load32(src));
    }

    skvm::Program p = b.done();
    REPORTER_ASSERT(r, p.hasJIT());

    int src[] = {1,2,3,4,5,6,7,8,9},
        dst[] = {0,0,0,0,0,0,0,0,0};

    p.eval(SK_ARRAY_COUNT(src)-1, src, dst);
    for (size_t i = 0; i < SK_ARRAY_COUNT(src)-1; i++) {
        REPORTER_ASSERT(r, dst[i] == src[i]);
    }
    size_t i = SK_ARRAY_COUNT(src)-1;
    REPORTER_ASSERT(r, dst[i] == 0);
}
#endif

DEF_TEST(SkVM_LoopCounts, r) {
    // Make sure we cover all the exact N we want.

    // buf[i] += 1
    skvm::Builder b;
    skvm::Arg arg = b.varying<int>();
    b.store32(arg,
              b.add(b.splat(1),
                    b.load32(arg)));

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int buf[64];
        for (int N = 0; N <= (int)SK_ARRAY_COUNT(buf); N++) {
            for (int i = 0; i < (int)SK_ARRAY_COUNT(buf); i++) {
                buf[i] = i;
            }
            program.eval(N, buf);

            for (int i = 0; i < N; i++) {
                REPORTER_ASSERT(r, buf[i] == i+1);
            }
            for (int i = N; i < (int)SK_ARRAY_COUNT(buf); i++) {
                REPORTER_ASSERT(r, buf[i] == i);
            }
        }
    });
}

DEF_TEST(SkVM_gather32, r) {
    skvm::Builder b;
    {
        skvm::Arg uniforms = b.uniform(),
                  buf      = b.varying<int>();
        skvm::I32 x = b.load32(buf);
        b.store32(buf, b.gather32(uniforms,0, b.bit_and(x, b.splat(7))));
    }

#if defined(SK_CPU_X86)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        const int img[] = {12,34,56,78, 90,98,76,54};

        int buf[20];
        for (int i = 0; i < 20; i++) {
            buf[i] = i;
        }

        struct Uniforms {
            const int* img;
        } uniforms{img};

        program.eval(20, &uniforms, buf);
        int i = 0;
        REPORTER_ASSERT(r, buf[i] == 12); i++;
        REPORTER_ASSERT(r, buf[i] == 34); i++;
        REPORTER_ASSERT(r, buf[i] == 56); i++;
        REPORTER_ASSERT(r, buf[i] == 78); i++;
        REPORTER_ASSERT(r, buf[i] == 90); i++;
        REPORTER_ASSERT(r, buf[i] == 98); i++;
        REPORTER_ASSERT(r, buf[i] == 76); i++;
        REPORTER_ASSERT(r, buf[i] == 54); i++;

        REPORTER_ASSERT(r, buf[i] == 12); i++;
        REPORTER_ASSERT(r, buf[i] == 34); i++;
        REPORTER_ASSERT(r, buf[i] == 56); i++;
        REPORTER_ASSERT(r, buf[i] == 78); i++;
        REPORTER_ASSERT(r, buf[i] == 90); i++;
        REPORTER_ASSERT(r, buf[i] == 98); i++;
        REPORTER_ASSERT(r, buf[i] == 76); i++;
        REPORTER_ASSERT(r, buf[i] == 54); i++;

        REPORTER_ASSERT(r, buf[i] == 12); i++;
        REPORTER_ASSERT(r, buf[i] == 34); i++;
        REPORTER_ASSERT(r, buf[i] == 56); i++;
        REPORTER_ASSERT(r, buf[i] == 78); i++;
    });
}

DEF_TEST(SkVM_gathers, r) {
    skvm::Builder b;
    {
        skvm::Arg uniforms = b.uniform(),
                  buf32    = b.varying<int>(),
                  buf16    = b.varying<uint16_t>(),
                  buf8     = b.varying<uint8_t>();

        skvm::I32 x = b.load32(buf32);

        b.store32(buf32, b.gather32(uniforms,0, b.bit_and(x, b.splat( 7))));
        b.store16(buf16, b.gather16(uniforms,0, b.bit_and(x, b.splat(15))));
        b.store8 (buf8 , b.gather8 (uniforms,0, b.bit_and(x, b.splat(31))));
    }

#if defined(SKVM_LLVM)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        const int img[] = {12,34,56,78, 90,98,76,54};

        constexpr int N = 20;
        int      buf32[N];
        uint16_t buf16[N];
        uint8_t  buf8 [N];

        for (int i = 0; i < 20; i++) {
            buf32[i] = i;
        }

        struct Uniforms {
            const int* img;
        } uniforms{img};

        program.eval(N, &uniforms, buf32, buf16, buf8);
        int i = 0;
        REPORTER_ASSERT(r, buf32[i] == 12 && buf16[i] == 12 && buf8[i] == 12); i++;
        REPORTER_ASSERT(r, buf32[i] == 34 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 56 && buf16[i] == 34 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 78 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 90 && buf16[i] == 56 && buf8[i] == 34); i++;
        REPORTER_ASSERT(r, buf32[i] == 98 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 76 && buf16[i] == 78 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 54 && buf16[i] ==  0 && buf8[i] ==  0); i++;

        REPORTER_ASSERT(r, buf32[i] == 12 && buf16[i] == 90 && buf8[i] == 56); i++;
        REPORTER_ASSERT(r, buf32[i] == 34 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 56 && buf16[i] == 98 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 78 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 90 && buf16[i] == 76 && buf8[i] == 78); i++;
        REPORTER_ASSERT(r, buf32[i] == 98 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 76 && buf16[i] == 54 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 54 && buf16[i] ==  0 && buf8[i] ==  0); i++;

        REPORTER_ASSERT(r, buf32[i] == 12 && buf16[i] == 12 && buf8[i] == 90); i++;
        REPORTER_ASSERT(r, buf32[i] == 34 && buf16[i] ==  0 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 56 && buf16[i] == 34 && buf8[i] ==  0); i++;
        REPORTER_ASSERT(r, buf32[i] == 78 && buf16[i] ==  0 && buf8[i] ==  0); i++;
    });
}

DEF_TEST(SkVM_bitops, r) {
    skvm::Builder b;
    {
        skvm::Arg ptr = b.varying<int>();

        skvm::I32 x = b.load32(ptr);

        x = b.bit_and  (x, b.splat(0xf1));  // 0x40
        x = b.bit_or   (x, b.splat(0x80));  // 0xc0
        x = b.bit_xor  (x, b.splat(0xfe));  // 0x3e
        x = b.bit_clear(x, b.splat(0x30));  // 0x0e

        x = b.shl(x, 28);  // 0xe000'0000
        x = b.sra(x, 28);  // 0xffff'fffe
        x = b.shr(x,  1);  // 0x7fff'ffff

        b.store32(ptr, x);
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int x = 0x42;
        program.eval(1, &x);
        REPORTER_ASSERT(r, x == 0x7fff'ffff);
    });
}

DEF_TEST(SkVM_select_is_NaN, r) {
    skvm::Builder b;
    {
        skvm::Arg src = b.varying<float>(),
                  dst = b.varying<float>();

        skvm::F32 x = b.loadF(src);
        x = select(is_NaN(x), b.splat(0.0f)
                            , x);
        b.storeF(dst, x);
    }

    std::vector<skvm::OptimizedInstruction> program = b.optimize();
    REPORTER_ASSERT(r, program.size() == 4);
    REPORTER_ASSERT(r, program[0].op == skvm::Op::load32);
    REPORTER_ASSERT(r, program[1].op == skvm::Op::neq_f32);
    REPORTER_ASSERT(r, program[2].op == skvm::Op::bit_clear);
    REPORTER_ASSERT(r, program[3].op == skvm::Op::store32);

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        // ±NaN, ±0, ±1, ±inf
        uint32_t src[] = {0x7f80'0001, 0xff80'0001, 0x0000'0000, 0x8000'0000,
                          0x3f80'0000, 0xbf80'0000, 0x7f80'0000, 0xff80'0000};
        uint32_t dst[SK_ARRAY_COUNT(src)];
        program.eval(SK_ARRAY_COUNT(src), src, dst);

        for (int i = 0; i < (int)SK_ARRAY_COUNT(src); i++) {
            REPORTER_ASSERT(r, dst[i] == (i < 2 ? 0 : src[i]));
        }
    });
}

DEF_TEST(SkVM_f32, r) {
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<float>();

        skvm::F32 x = b.loadF(arg),
                  y = b.add(x,x),   // y = 2x
                  z = b.sub(y,x),   // z = 2x-x = x
                  w = b.div(z,x);   // w = x/x = 1
        b.storeF(arg, w);
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        float buf[] = { 1,2,3,4,5,6,7,8,9 };
        program.eval(SK_ARRAY_COUNT(buf), buf);
        for (float v : buf) {
            REPORTER_ASSERT(r, v == 1.0f);
        }
    });
}

DEF_TEST(SkVM_cmp_i32, r) {
    skvm::Builder b;
    {
        skvm::I32 x = b.load32(b.varying<int>());

        auto to_bit = [&](int shift, skvm::I32 mask) {
            return b.shl(b.bit_and(mask, b.splat(0x1)), shift);
        };

        skvm::I32 m = b.splat(0);
        m = b.bit_or(m, to_bit(0, b. eq(x, b.splat(0))));
        m = b.bit_or(m, to_bit(1, b.neq(x, b.splat(1))));
        m = b.bit_or(m, to_bit(2, b. lt(x, b.splat(2))));
        m = b.bit_or(m, to_bit(3, b.lte(x, b.splat(3))));
        m = b.bit_or(m, to_bit(4, b. gt(x, b.splat(4))));
        m = b.bit_or(m, to_bit(5, b.gte(x, b.splat(5))));

        b.store32(b.varying<int>(), m);
    }
#if defined(SKVM_LLVM)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        int in[] = { 0,1,2,3,4,5,6,7,8,9 };
        int out[SK_ARRAY_COUNT(in)];

        program.eval(SK_ARRAY_COUNT(in), in, out);

        REPORTER_ASSERT(r, out[0] == 0b001111);
        REPORTER_ASSERT(r, out[1] == 0b001100);
        REPORTER_ASSERT(r, out[2] == 0b001010);
        REPORTER_ASSERT(r, out[3] == 0b001010);
        REPORTER_ASSERT(r, out[4] == 0b000010);
        for (int i = 5; i < (int)SK_ARRAY_COUNT(out); i++) {
            REPORTER_ASSERT(r, out[i] == 0b110010);
        }
    });
}

DEF_TEST(SkVM_cmp_f32, r) {
    skvm::Builder b;
    {
        skvm::F32 x = b.loadF(b.varying<float>());

        auto to_bit = [&](int shift, skvm::I32 mask) {
            return b.shl(b.bit_and(mask, b.splat(0x1)), shift);
        };

        skvm::I32 m = b.splat(0);
        m = b.bit_or(m, to_bit(0, b. eq(x, b.splat(0.0f))));
        m = b.bit_or(m, to_bit(1, b.neq(x, b.splat(1.0f))));
        m = b.bit_or(m, to_bit(2, b. lt(x, b.splat(2.0f))));
        m = b.bit_or(m, to_bit(3, b.lte(x, b.splat(3.0f))));
        m = b.bit_or(m, to_bit(4, b. gt(x, b.splat(4.0f))));
        m = b.bit_or(m, to_bit(5, b.gte(x, b.splat(5.0f))));

        b.store32(b.varying<int>(), m);
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        float in[] = { 0,1,2,3,4,5,6,7,8,9 };
        int out[SK_ARRAY_COUNT(in)];

        program.eval(SK_ARRAY_COUNT(in), in, out);

        REPORTER_ASSERT(r, out[0] == 0b001111);
        REPORTER_ASSERT(r, out[1] == 0b001100);
        REPORTER_ASSERT(r, out[2] == 0b001010);
        REPORTER_ASSERT(r, out[3] == 0b001010);
        REPORTER_ASSERT(r, out[4] == 0b000010);
        for (int i = 5; i < (int)SK_ARRAY_COUNT(out); i++) {
            REPORTER_ASSERT(r, out[i] == 0b110010);
        }
    });
}

DEF_TEST(SkVM_index, r) {
    skvm::Builder b;
    b.store32(b.varying<int>(), b.index());

#if defined(SKVM_LLVM) || defined(SK_CPU_X86)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        int buf[23];
        program.eval(SK_ARRAY_COUNT(buf), buf);
        for (int i = 0; i < (int)SK_ARRAY_COUNT(buf); i++) {
            REPORTER_ASSERT(r, buf[i] == (int)SK_ARRAY_COUNT(buf)-i);
        }
    });
}

DEF_TEST(SkVM_i16x2, r) {
    skvm::Builder b;
    {
        skvm::Arg buf = b.varying<int>();

        skvm::I32 x = b.load32(buf),
                  y = b.add_16x2(x,x),   // y = 2x
                  z = b.mul_16x2(x,y),   // z = 2x^2
                  w = b.sub_16x2(z,x),   // w = x(2x-1)
                  v = b.shl_16x2(w,7),   // These shifts will be a no-op
                  u = b.sra_16x2(v,7);   // for all but x=12 and x=13.
        b.store32(buf, u);
    }

#if defined(SKVM_LLVM)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        uint16_t buf[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13 };

        program.eval(SK_ARRAY_COUNT(buf)/2, buf);
        for (int i = 0; i < 12; i++) {
            REPORTER_ASSERT(r, buf[i] == i*(2*i-1));
        }
        REPORTER_ASSERT(r, buf[12] == 0xff14);   // 12*23 = 0x114
        REPORTER_ASSERT(r, buf[13] == 0xff45);   // 13*25 = 0x145
    });
}

DEF_TEST(SkVM_cmp_i16, r) {
    skvm::Builder b;
    {
        skvm::Arg buf = b.varying<int>();
        skvm::I32 x = b.load32(buf);

        auto to_bit = [&](int shift, skvm::I32 mask) {
            return b.shl_16x2(b.bit_and(mask, b.splat(0x0001'0001)), shift);
        };

        skvm::I32 m = b.splat(0);
        m = b.bit_or(m, to_bit(0, b. eq_16x2(x, b.splat(0x0000'0000))));
        m = b.bit_or(m, to_bit(1, b.neq_16x2(x, b.splat(0x0001'0001))));
        m = b.bit_or(m, to_bit(2, b. lt_16x2(x, b.splat(0x0002'0002))));
        m = b.bit_or(m, to_bit(3, b.lte_16x2(x, b.splat(0x0003'0003))));
        m = b.bit_or(m, to_bit(4, b. gt_16x2(x, b.splat(0x0004'0004))));
        m = b.bit_or(m, to_bit(5, b.gte_16x2(x, b.splat(0x0005'0005))));

        b.store32(buf, m);
    }

#if defined(SKVM_LLVM)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        int16_t buf[] = { 0,1, 2,3, 4,5, 6,7, 8,9 };

        program.eval(SK_ARRAY_COUNT(buf)/2, buf);

        REPORTER_ASSERT(r, buf[0] == 0b001111);
        REPORTER_ASSERT(r, buf[1] == 0b001100);
        REPORTER_ASSERT(r, buf[2] == 0b001010);
        REPORTER_ASSERT(r, buf[3] == 0b001010);
        REPORTER_ASSERT(r, buf[4] == 0b000010);
        for (int i = 5; i < (int)SK_ARRAY_COUNT(buf); i++) {
            REPORTER_ASSERT(r, buf[i] == 0b110010);
        }
    });
}


DEF_TEST(SkVM_mad, r) {
    // This program is designed to exercise the tricky corners of instruction
    // and register selection for Op::mad_f32.

    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<int>();

        skvm::F32 x = b.to_f32(b.load32(arg)),
                  y = b.mad(x,x,x),   // x is needed in the future, so r[x] != r[y].
                  z = b.mad(y,y,x),   // y is needed in the future, but r[z] = r[x] is ok.
                  w = b.mad(z,z,y),   // w can alias z but not y.
                  v = b.mad(w,y,w);   // Got to stop somewhere.
        b.store32(arg, b.trunc(v));
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int x = 2;
        program.eval(1, &x);
        // x = 2
        // y = 2*2 + 2 = 6
        // z = 6*6 + 2 = 38
        // w = 38*38 + 6 = 1450
        // v = 1450*6 + 1450 = 10150
        REPORTER_ASSERT(r, x == 10150);
    });
}

DEF_TEST(SkVM_fms, r) {
    // Create a pattern that can be peepholed into an Op::fms_f32.
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<int>();

        skvm::F32 x = b.to_f32(b.load32(arg)),
                  v = b.sub(b.mul(x, b.splat(2.0f)),
                            b.splat(1.0f));
        b.store32(arg, b.trunc(v));
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int buf[] = {0,1,2,3,4,5,6,7,8,9,10};
        program.eval((int)SK_ARRAY_COUNT(buf), &buf);

        for (int i = 0; i < (int)SK_ARRAY_COUNT(buf); i++) {
            REPORTER_ASSERT(r, buf[i] = 2*i-1);
        }
    });
}

DEF_TEST(SkVM_fnma, r) {
    // Create a pattern that can be peepholed into an Op::fnma_f32.
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<int>();

        skvm::F32 x = b.to_f32(b.load32(arg)),
                  v = b.sub(b.splat(1.0f),
                            b.mul(x, b.splat(2.0f)));
        b.store32(arg, b.trunc(v));
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int buf[] = {0,1,2,3,4,5,6,7,8,9,10};
        program.eval((int)SK_ARRAY_COUNT(buf), &buf);

        for (int i = 0; i < (int)SK_ARRAY_COUNT(buf); i++) {
            REPORTER_ASSERT(r, buf[i] = 1-2*i);
        }
    });
}

DEF_TEST(SkVM_madder, r) {
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<float>();

        skvm::F32 x = b.loadF(arg),
                  y = b.mad(x,x,x),   // x is needed in the future, so r[x] != r[y].
                  z = b.mad(y,x,y),   // r[x] can be reused after this instruction, but not r[y].
                  w = b.mad(y,y,z);
        b.storeF(arg, w);
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        float x = 2.0f;
        // y = 2*2 + 2 = 6
        // z = 6*2 + 6 = 18
        // w = 6*6 + 18 = 54
        program.eval(1, &x);
        REPORTER_ASSERT(r, x == 54.0f);
    });
}

DEF_TEST(SkVM_floor, r) {
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<float>();
        b.storeF(arg, b.floor(b.loadF(arg)));
    }

#if defined(SK_CPU_X86)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        float buf[]  = { -2.0f, -1.5f, -1.0f, 0.0f, 1.0f, 1.5f, 2.0f };
        float want[] = { -2.0f, -2.0f, -1.0f, 0.0f, 1.0f, 1.0f, 2.0f };
        program.eval(SK_ARRAY_COUNT(buf), buf);
        for (int i = 0; i < (int)SK_ARRAY_COUNT(buf); i++) {
            REPORTER_ASSERT(r, buf[i] == want[i]);
        }
    });
}

DEF_TEST(SkVM_round, r) {
    skvm::Builder b;
    {
        skvm::Arg src = b.varying<float>();
        skvm::Arg dst = b.varying<int>();
        b.store32(dst, b.round(b.loadF(src)));
    }

    // The test cases on exact 0.5f boundaries assume the current rounding mode is nearest even.
    // We haven't explicitly guaranteed that here... it just probably is.
    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        float buf[]  = { -1.5f, -0.5f, 0.0f, 0.5f, 0.2f, 0.6f, 1.0f, 1.4f, 1.5f, 2.0f };
        int want[] =   { -2   ,  0   , 0   , 0   , 0   , 1   , 1   , 1   , 2   , 2    };
        int dst[SK_ARRAY_COUNT(buf)];

        program.eval(SK_ARRAY_COUNT(buf), buf, dst);
        for (int i = 0; i < (int)SK_ARRAY_COUNT(dst); i++) {
            REPORTER_ASSERT(r, dst[i] == want[i]);
        }
    });
}

DEF_TEST(SkVM_min, r) {
    skvm::Builder b;
    {
        skvm::Arg src1 = b.varying<float>();
        skvm::Arg src2 = b.varying<float>();
        skvm::Arg dst = b.varying<float>();

        b.storeF(dst, b.min(b.loadF(src1), b.loadF(src2)));
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        float s1[]  =  { 0.0f, 1.0f, 4.0f, -1.0f, -1.0f};
        float s2[]  =  { 0.0f, 2.0f, 3.0f,  1.0f, -2.0f};
        float want[] = { 0.0f, 1.0f, 3.0f, -1.0f, -2.0f};
        float d[SK_ARRAY_COUNT(s1)];
        program.eval(SK_ARRAY_COUNT(d), s1, s2, d);
        for (int i = 0; i < (int)SK_ARRAY_COUNT(d); i++) {
          REPORTER_ASSERT(r, d[i] == want[i]);
        }
    });
}

DEF_TEST(SkVM_max, r) {
    skvm::Builder b;
    {
        skvm::Arg src1 = b.varying<float>();
        skvm::Arg src2 = b.varying<float>();
        skvm::Arg dst = b.varying<float>();

        b.storeF(dst, b.max(b.loadF(src1), b.loadF(src2)));
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        float s1[]  =  { 0.0f, 1.0f, 4.0f, -1.0f, -1.0f};
        float s2[]  =  { 0.0f, 2.0f, 3.0f,  1.0f, -2.0f};
        float want[] = { 0.0f, 2.0f, 4.0f,  1.0f, -1.0f};
        float d[SK_ARRAY_COUNT(s1)];
        program.eval(SK_ARRAY_COUNT(d), s1, s2, d);
        for (int i = 0; i < (int)SK_ARRAY_COUNT(d); i++) {
          REPORTER_ASSERT(r, d[i] == want[i]);
        }
    });
}

DEF_TEST(SkVM_hoist, r) {
    // This program uses enough constants that it will fail to JIT if we hoist them.
    // The JIT will try again without hoisting, and that'll just need 2 registers.
    skvm::Builder b;
    {
        skvm::Arg arg = b.varying<int>();
        skvm::I32 x = b.load32(arg);
        for (int i = 0; i < 32; i++) {
            x = b.add(x, b.splat(i));
        }
        b.store32(arg, x);
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int x = 4;
        program.eval(1, &x);
        // x += 0 + 1 + 2 + 3 + ... + 30 + 31
        // x += 496
        REPORTER_ASSERT(r, x == 500);
    });
}

DEF_TEST(SkVM_select, r) {
    skvm::Builder b;
    {
        skvm::Arg buf = b.varying<int>();

        skvm::I32 x = b.load32(buf);

        x = b.select( b.gt(x, b.splat(4)), x, b.splat(42) );

        b.store32(buf, x);
    }

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int buf[] = { 0,1,2,3,4,5,6,7,8 };
        program.eval(SK_ARRAY_COUNT(buf), buf);
        for (int i = 0; i < (int)SK_ARRAY_COUNT(buf); i++) {
            REPORTER_ASSERT(r, buf[i] == (i > 4 ? i : 42));
        }
    });
}

DEF_TEST(SkVM_NewOps, r) {
    // Exercise a somewhat arbitrary set of new ops.
    skvm::Builder b;
    {
        skvm::Arg buf      = b.varying<int16_t>(),
                  uniforms = b.uniform();

        skvm::I32 x = b.load16(buf);

        const size_t kPtr = sizeof(const int*);

        x = b.add(x, b.uniform32(uniforms, kPtr+0));
        x = b.mul(x, b.uniform8 (uniforms, kPtr+4));
        x = b.sub(x, b.uniform16(uniforms, kPtr+6));

        skvm::I32 limit = b.uniform32(uniforms, kPtr+8);
        x = b.select(b.lt(x, b.splat(0)), b.splat(0), x);
        x = b.select(b.gt(x, limit     ), limit     , x);

        x = b.gather8(uniforms,0, x);

        b.store16(buf, x);
    }

    if ((false)) {
        SkDynamicMemoryWStream buf;
        dump(b, &buf);
        sk_sp<SkData> blob = buf.detachAsData();
        SkDebugf("%.*s\n", blob->size(), blob->data());
    }

#if defined(SKVM_LLVM)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        const int N = 31;
        int16_t buf[N];
        for (int i = 0; i < N; i++) {
            buf[i] = i;
        }

        const int M = 16;
        uint8_t img[M];
        for (int i = 0; i < M; i++) {
            img[i] = i*i;
        }

        struct {
            const uint8_t* img;
            int      add   = 5;
            uint8_t  mul   = 3;
            uint16_t sub   = 18;
            int      limit = M-1;
        } uniforms{img};

        program.eval(N, buf, &uniforms);

        for (int i = 0; i < N; i++) {
            // Our first math calculates x = (i+5)*3 - 18 a.k.a 3*(i-1).
            int x = 3*(i-1);

            // Then that's pinned to the limits of img.
            if (i < 2) { x =  0; }  // Notice i == 1 hits x == 0 exactly...
            if (i > 5) { x = 15; }  // ...and i == 6 hits x == 15 exactly
            REPORTER_ASSERT(r, buf[i] == img[x]);
        }
    });
}

DEF_TEST(SkVM_sqrt, r) {
    skvm::Builder b;
    auto buf = b.varying<int>();
    b.storeF(buf, b.sqrt(b.loadF(buf)));

#if defined(SKVM_LLVM) || defined(SK_CPU_X86)
    test_jit_and_interpreter
#else
    test_interpreter_only
#endif
    (r, b.done(), [&](const skvm::Program& program) {
        constexpr int K = 17;
        float buf[K];
        for (int i = 0; i < K; i++) {
            buf[i] = (float)(i*i);
        }

        // x^2 -> x
        program.eval(K, buf);

        for (int i = 0; i < K; i++) {
            REPORTER_ASSERT(r, buf[i] == (float)i);
        }
    });
}

DEF_TEST(SkVM_MSAN, r) {
    // This little memset32() program should be able to JIT, but if we run that
    // JIT code in an MSAN build, it won't see the writes initialize buf.  So
    // this tests that we're using the interpreter instead.
    skvm::Builder b;
    b.store32(b.varying<int>(), b.splat(42));

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        constexpr int K = 17;
        int buf[K];                 // Intentionally uninitialized.
        program.eval(K, buf);
        sk_msan_assert_initialized(buf, buf+K);
        for (int x : buf) {
            REPORTER_ASSERT(r, x == 42);
        }
    });
}

DEF_TEST(SkVM_assert, r) {
    skvm::Builder b;
    b.assert_true(b.lt(b.load32(b.varying<int>()),
                       b.splat(42)));

    test_jit_and_interpreter(r, b.done(), [&](const skvm::Program& program) {
        int buf[] = { 0,1,2,3,4,5,6,7,8,9 };
        program.eval(SK_ARRAY_COUNT(buf), buf);
    });
}

DEF_TEST(SkVM_premul, reporter) {
    // Test that premul is short-circuited when alpha is known opaque.
    {
        skvm::Builder p;
        auto rptr = p.varying<int>(),
             aptr = p.varying<int>();

        skvm::F32 r = p.loadF(rptr),
                  g = p.splat(0.0f),
                  b = p.splat(0.0f),
                  a = p.loadF(aptr);

        p.premul(&r, &g, &b, a);
        p.storeF(rptr, r);

        // load red, load alpha, red *= alpha, store red
        REPORTER_ASSERT(reporter, p.done().instructions().size() == 4);
    }

    {
        skvm::Builder p;
        auto rptr = p.varying<int>();

        skvm::F32 r = p.loadF(rptr),
                  g = p.splat(0.0f),
                  b = p.splat(0.0f),
                  a = p.splat(1.0f);

        p.premul(&r, &g, &b, a);
        p.storeF(rptr, r);

        // load red, store red
        REPORTER_ASSERT(reporter, p.done().instructions().size() == 2);
    }

    // Same deal for unpremul.
    {
        skvm::Builder p;
        auto rptr = p.varying<int>(),
             aptr = p.varying<int>();

        skvm::F32 r = p.loadF(rptr),
                  g = p.splat(0.0f),
                  b = p.splat(0.0f),
                  a = p.loadF(aptr);

        p.unpremul(&r, &g, &b, a);
        p.storeF(rptr, r);

        // load red, load alpha, a bunch of unpremul instructions, store red
        REPORTER_ASSERT(reporter, p.done().instructions().size() >= 4);
    }

    {
        skvm::Builder p;
        auto rptr = p.varying<int>();

        skvm::F32 r = p.loadF(rptr),
                  g = p.splat(0.0f),
                  b = p.splat(0.0f),
                  a = p.splat(1.0f);

        p.unpremul(&r, &g, &b, a);
        p.storeF(rptr, r);

        // load red, store red
        REPORTER_ASSERT(reporter, p.done().instructions().size() == 2);
    }
}

template <typename Fn>
static void test_asm(skiatest::Reporter* r, Fn&& fn, std::initializer_list<uint8_t> expected) {
    uint8_t buf[4096];
    skvm::Assembler a{buf};
    fn(a);

    REPORTER_ASSERT(r, a.size() == expected.size());

    auto got = (const uint8_t*)buf,
         want = expected.begin();
    for (int i = 0; i < (int)std::min(a.size(), expected.size()); i++) {
        REPORTER_ASSERT(r, got[i] == want[i],
                        "byte %d was %02x, want %02x", i, got[i], want[i]);
    }
}

DEF_TEST(SkVM_Assembler, r) {
    // Easiest way to generate test cases is
    //
    //   echo '...some asm...' | llvm-mc -show-encoding -x86-asm-syntax=intel
    //
    // The -x86-asm-syntax=intel bit is optional, controlling the
    // input syntax only; the output will always be AT&T  op x,y,dst style.
    // Our APIs read more like Intel op dst,x,y as op(dst,x,y), so I find
    // that a bit easier to use here, despite maybe favoring AT&T overall.

    using A = skvm::Assembler;
    // Our exit strategy from AVX code.
    test_asm(r, [&](A& a) {
        a.int3();
        a.vzeroupper();
        a.ret();
    },{
        0xcc,
        0xc5, 0xf8, 0x77,
        0xc3,
    });

    // Align should pad with zero
    test_asm(r, [&](A& a) {
        a.ret();
        a.align(4);
    },{
        0xc3,
        0x00, 0x00, 0x00,
    });

    test_asm(r, [&](A& a) {
        a.add(A::rax, 8);       // Always good to test rax.
        a.sub(A::rax, 32);

        a.add(A::rdi, 12);      // Last 0x48 REX
        a.sub(A::rdi, 8);

        a.add(A::r8 , 7);       // First 0x49 REX
        a.sub(A::r8 , 4);

        a.add(A::rsi, 128);     // Requires 4 byte immediate.
        a.sub(A::r8 , 1000000);
    },{
        0x48, 0x83, 0b11'000'000, 0x08,
        0x48, 0x83, 0b11'101'000, 0x20,

        0x48, 0x83, 0b11'000'111, 0x0c,
        0x48, 0x83, 0b11'101'111, 0x08,

        0x49, 0x83, 0b11'000'000, 0x07,
        0x49, 0x83, 0b11'101'000, 0x04,

        0x48, 0x81, 0b11'000'110, 0x80, 0x00, 0x00, 0x00,
        0x49, 0x81, 0b11'101'000, 0x40, 0x42, 0x0f, 0x00,
    });


    test_asm(r, [&](A& a) {
        a.vpaddd (A::ymm0, A::ymm1, A::ymm2);  // Low registers and 0x0f map     -> 2-byte VEX.
        a.vpaddd (A::ymm8, A::ymm1, A::ymm2);  // A high dst register is ok      -> 2-byte VEX.
        a.vpaddd (A::ymm0, A::ymm8, A::ymm2);  // A high first argument register -> 2-byte VEX.
        a.vpaddd (A::ymm0, A::ymm1, A::ymm8);  // A high second argument         -> 3-byte VEX.
        a.vpmulld(A::ymm0, A::ymm1, A::ymm2);  // Using non-0x0f map instruction -> 3-byte VEX.
        a.vpsubd (A::ymm0, A::ymm1, A::ymm2);  // Test vpsubd to ensure argument order is right.
    },{
        /*    VEX     */ /*op*/ /*modRM*/
        0xc5,       0xf5, 0xfe, 0xc2,
        0xc5,       0x75, 0xfe, 0xc2,
        0xc5,       0xbd, 0xfe, 0xc2,
        0xc4, 0xc1, 0x75, 0xfe, 0xc0,
        0xc4, 0xe2, 0x75, 0x40, 0xc2,
        0xc5,       0xf5, 0xfa, 0xc2,
    });

    test_asm(r, [&](A& a) {
        a.vpcmpeqd (A::ymm0, A::ymm1, A::ymm2);
        a.vpcmpgtd (A::ymm0, A::ymm1, A::ymm2);
        a.vcmpeqps (A::ymm0, A::ymm1, A::ymm2);
        a.vcmpltps (A::ymm0, A::ymm1, A::ymm2);
        a.vcmpleps (A::ymm0, A::ymm1, A::ymm2);
        a.vcmpneqps(A::ymm0, A::ymm1, A::ymm2);
    },{
        0xc5,0xf5,0x76,0xc2,
        0xc5,0xf5,0x66,0xc2,
        0xc5,0xf4,0xc2,0xc2,0x00,
        0xc5,0xf4,0xc2,0xc2,0x01,
        0xc5,0xf4,0xc2,0xc2,0x02,
        0xc5,0xf4,0xc2,0xc2,0x04,
    });

    test_asm(r, [&](A& a) {
        a.vminps(A::ymm0, A::ymm1, A::ymm2);
        a.vmaxps(A::ymm0, A::ymm1, A::ymm2);
    },{
        0xc5,0xf4,0x5d,0xc2,
        0xc5,0xf4,0x5f,0xc2,
    });

    test_asm(r, [&](A& a) {
        a.vpblendvb(A::ymm0, A::ymm1, A::ymm2, A::ymm3);
    },{
        0xc4,0xe3,0x75, 0x4c, 0xc2, 0x30,
    });

    test_asm(r, [&](A& a) {
        a.vpsrld(A::ymm15, A::ymm2, 8);
        a.vpsrld(A::ymm0 , A::ymm8, 5);
    },{
        0xc5,     0x85, 0x72,0xd2, 0x08,
        0xc4,0xc1,0x7d, 0x72,0xd0, 0x05,
    });

    test_asm(r, [&](A& a) {
        a.vpermq(A::ymm1, A::ymm2, 5);
    },{
        0xc4,0xe3,0xfd, 0x00,0xca, 0x05,
    });

    test_asm(r, [&](A& a) {
        a.vroundps(A::ymm1, A::ymm2, A::NEAREST);
        a.vroundps(A::ymm1, A::ymm2, A::FLOOR);
        a.vroundps(A::ymm1, A::ymm2, A::CEIL);
        a.vroundps(A::ymm1, A::ymm2, A::TRUNC);
    },{
        0xc4,0xe3,0x7d,0x08,0xca,0x00,
        0xc4,0xe3,0x7d,0x08,0xca,0x01,
        0xc4,0xe3,0x7d,0x08,0xca,0x02,
        0xc4,0xe3,0x7d,0x08,0xca,0x03,
    });

    test_asm(r, [&](A& a) {
        A::Label l = a.here();
        a.byte(1);
        a.byte(2);
        a.byte(3);
        a.byte(4);

        a.vbroadcastss(A::ymm0 , &l);
        a.vbroadcastss(A::ymm1 , &l);
        a.vbroadcastss(A::ymm8 , &l);
        a.vbroadcastss(A::ymm15, &l);

        a.vpshufb(A::ymm4, A::ymm3, &l);
        a.vpaddd (A::ymm4, A::ymm3, &l);
        a.vpsubd (A::ymm4, A::ymm3, &l);

        a.vptest(A::ymm4, &l);

        a.vmulps (A::ymm4, A::ymm3, &l);
    },{
        0x01, 0x02, 0x03, 0x4,

        /*     VEX    */  /*op*/ /*   ModRM    */  /*     offset     */
        0xc4, 0xe2, 0x7d,  0x18,   0b00'000'101,   0xf3,0xff,0xff,0xff,   // 0xfffffff3 == -13
        0xc4, 0xe2, 0x7d,  0x18,   0b00'001'101,   0xea,0xff,0xff,0xff,   // 0xffffffea == -22
        0xc4, 0x62, 0x7d,  0x18,   0b00'000'101,   0xe1,0xff,0xff,0xff,   // 0xffffffe1 == -31
        0xc4, 0x62, 0x7d,  0x18,   0b00'111'101,   0xd8,0xff,0xff,0xff,   // 0xffffffd8 == -40

        0xc4, 0xe2, 0x65,  0x00,   0b00'100'101,   0xcf,0xff,0xff,0xff,   // 0xffffffcf == -49

        0xc5, 0xe5,        0xfe,   0b00'100'101,   0xc7,0xff,0xff,0xff,   // 0xffffffc7 == -57
        0xc5, 0xe5,        0xfa,   0b00'100'101,   0xbf,0xff,0xff,0xff,   // 0xffffffbf == -65

        0xc4, 0xe2, 0x7d,  0x17,   0b00'100'101,   0xb6,0xff,0xff,0xff,   // 0xffffffb6 == -74

        0xc5, 0xe4,        0x59,   0b00'100'101,   0xae,0xff,0xff,0xff,   // 0xffffffaf == -82
    });

    test_asm(r, [&](A& a) {
        a.vbroadcastss(A::ymm0,  A::rdi,   0);
        a.vbroadcastss(A::ymm13, A::r14,   7);
        a.vbroadcastss(A::ymm8,  A::rdx, -12);
        a.vbroadcastss(A::ymm8,  A::rdx, 400);

        a.vbroadcastss(A::ymm8,  A::xmm0);
        a.vbroadcastss(A::ymm0,  A::xmm13);
    },{
        /*   VEX    */ /*op*/     /*ModRM*/   /*offset*/
        0xc4,0xe2,0x7d, 0x18,   0b00'000'111,
        0xc4,0x42,0x7d, 0x18,   0b01'101'110,  0x07,
        0xc4,0x62,0x7d, 0x18,   0b01'000'010,  0xf4,
        0xc4,0x62,0x7d, 0x18,   0b10'000'010,  0x90,0x01,0x00,0x00,

        0xc4,0x62,0x7d, 0x18,   0b11'000'000,
        0xc4,0xc2,0x7d, 0x18,   0b11'000'101,
    });

    test_asm(r, [&](A& a) {
        A::Label l = a.here();
        a.jne(&l);
        a.jne(&l);
        a.je (&l);
        a.jmp(&l);
        a.jl (&l);
        a.jc (&l);

        a.cmp(A::rdx, 0);
        a.cmp(A::rax, 12);
        a.cmp(A::r14, 2000000000);
    },{
        0x0f,0x85, 0xfa,0xff,0xff,0xff,   // near jne -6 bytes
        0x0f,0x85, 0xf4,0xff,0xff,0xff,   // near jne -12 bytes
        0x0f,0x84, 0xee,0xff,0xff,0xff,   // near je  -18 bytes
        0xe9,      0xe9,0xff,0xff,0xff,   // near jmp -23 bytes
        0x0f,0x8c, 0xe3,0xff,0xff,0xff,   // near jl  -29 bytes
        0x0f,0x82, 0xdd,0xff,0xff,0xff,   // near jc  -35 bytes

        0x48,0x83,0xfa,0x00,
        0x48,0x83,0xf8,0x0c,
        0x49,0x81,0xfe,0x00,0x94,0x35,0x77,
    });

    test_asm(r, [&](A& a) {
        a.vmovups(A::ymm5, A::rsi);
        a.vmovups(A::rsi, A::ymm5);

        a.vmovups(A::rsi, A::xmm5);

        a.vpmovzxwd(A::ymm4, A::rsi);
        a.vpmovzxbd(A::ymm4, A::rsi);

        a.vmovq(A::rdx, A::xmm15);
    },{
        /*    VEX    */  /*Op*/  /*  ModRM  */
        0xc5,     0xfc,   0x10,  0b00'101'110,
        0xc5,     0xfc,   0x11,  0b00'101'110,

        0xc5,     0xf8,   0x11,  0b00'101'110,

        0xc4,0xe2,0x7d,   0x33,  0b00'100'110,
        0xc4,0xe2,0x7d,   0x31,  0b00'100'110,

        0xc5,     0x79,   0xd6,  0b00'111'010,
    });

    test_asm(r, [&](A& a) {
        a.movzbl(A::rax, A::rsi, 0);   // Low registers for src and dst.
        a.movzbl(A::rax, A::r8,  0);   // High src register.
        a.movzbl(A::r8 , A::rsi, 0);   // High dst register.
        a.movzbl(A::r8,  A::rsi, 12);
        a.movzbl(A::r8,  A::rsi, 400);

        a.vmovd(A::rax, A::xmm0);
        a.vmovd(A::rax, A::xmm8);
        a.vmovd(A::r8,  A::xmm0);

        a.vmovd(A::xmm0, A::rax);
        a.vmovd(A::xmm8, A::rax);
        a.vmovd(A::xmm0, A::r8);

        a.vmovd(A::xmm0 , A::FOUR, A::rcx, A::rax);
        a.vmovd(A::xmm15, A::TWO,  A::r8,  A::rax);
        a.vmovd(A::xmm0 , A::ONE,  A::rcx, A::r8);

        a.vmovd_direct(A::rax, A::xmm0);
        a.vmovd_direct(A::rax, A::xmm8);
        a.vmovd_direct(A::r8,  A::xmm0);

        a.vmovd_direct(A::xmm0, A::rax);
        a.vmovd_direct(A::xmm8, A::rax);
        a.vmovd_direct(A::xmm0, A::r8);

        a.movb(A::rdx, A::rax);
        a.movb(A::rdx, A::r8);
        a.movb(A::r8 , A::rax);
    },{
        0x0f,0xb6,0x06,
        0x41,0x0f,0xb6,0x00,
        0x44,0x0f,0xb6,0x06,
        0x44,0x0f,0xb6,0x46, 12,
        0x44,0x0f,0xb6,0x86, 0x90,0x01,0x00,0x00,

        0xc5,0xf9,0x7e,0x00,
        0xc5,0x79,0x7e,0x00,
        0xc4,0xc1,0x79,0x7e,0x00,

        0xc5,0xf9,0x6e,0x00,
        0xc5,0x79,0x6e,0x00,
        0xc4,0xc1,0x79,0x6e,0x00,

        0xc5,0xf9,0x6e,0x04,0x88,
        0xc4,0x21,0x79,0x6e,0x3c,0x40,
        0xc4,0xc1,0x79,0x6e,0x04,0x08,

        0xc5,0xf9,0x7e,0xc0,
        0xc5,0x79,0x7e,0xc0,
        0xc4,0xc1,0x79,0x7e,0xc0,

        0xc5,0xf9,0x6e,0xc0,
        0xc5,0x79,0x6e,0xc0,
        0xc4,0xc1,0x79,0x6e,0xc0,

        0x88, 0x02,
        0x44, 0x88, 0x02,
        0x41, 0x88, 0x00,
    });

    test_asm(r, [&](A& a) {
        a.vpinsrw(A::xmm1, A::xmm8, A::rsi, 4);
        a.vpinsrw(A::xmm8, A::xmm1, A::r8, 12);

        a.vpinsrb(A::xmm1, A::xmm8, A::rsi, 4);
        a.vpinsrb(A::xmm8, A::xmm1, A::r8, 12);

        a.vpextrw(A::rsi, A::xmm8, 7);
        a.vpextrw(A::r8,  A::xmm1, 15);

        a.vpextrb(A::rsi, A::xmm8, 7);
        a.vpextrb(A::r8,  A::xmm1, 15);
    },{
        0xc5,0xb9,      0xc4, 0x0e,  4,
        0xc4,0x41,0x71, 0xc4, 0x00, 12,

        0xc4,0xe3,0x39, 0x20, 0x0e,  4,
        0xc4,0x43,0x71, 0x20, 0x00, 12,

        0xc4,0x63,0x79, 0x15, 0x06,  7,
        0xc4,0xc3,0x79, 0x15, 0x08, 15,

        0xc4,0x63,0x79, 0x14, 0x06,  7,
        0xc4,0xc3,0x79, 0x14, 0x08, 15,
    });

    test_asm(r, [&](A& a) {
        a.vpandn(A::ymm3, A::ymm12, A::ymm2);
    },{
        0xc5, 0x9d, 0xdf, 0xda,
    });

    test_asm(r, [&](A& a) {
        a.vmovdqa   (A::ymm3, A::ymm2);
        a.vcvttps2dq(A::ymm3, A::ymm2);
        a.vcvtdq2ps (A::ymm3, A::ymm2);
        a.vcvtps2dq (A::ymm3, A::ymm2);
        a.vsqrtps   (A::ymm3, A::ymm2);
    },{
        0xc5,0xfd,0x6f,0xda,
        0xc5,0xfe,0x5b,0xda,
        0xc5,0xfc,0x5b,0xda,
        0xc5,0xfd,0x5b,0xda,
        0xc5,0xfc,0x51,0xda,
    });

    test_asm(r, [&](A& a) {
        a.vgatherdps(A::ymm1 , A::FOUR , A::ymm0 , A::rdi, A::ymm2 );
        a.vgatherdps(A::ymm0 , A::ONE  , A::ymm2 , A::rax, A::ymm1 );
        a.vgatherdps(A::ymm10, A::ONE  , A::ymm2 , A::rax, A::ymm1 );
        a.vgatherdps(A::ymm0 , A::ONE  , A::ymm12, A::rax, A::ymm1 );
        a.vgatherdps(A::ymm0 , A::ONE  , A::ymm2 , A::r9 , A::ymm1 );
        a.vgatherdps(A::ymm0 , A::ONE  , A::ymm2 , A::rax, A::ymm12);
        a.vgatherdps(A::ymm0 , A::EIGHT, A::ymm2 , A::rax, A::ymm12);
    },{
        0xc4,0xe2,0x6d,0x92,0x0c,0x87,
        0xc4,0xe2,0x75,0x92,0x04,0x10,
        0xc4,0x62,0x75,0x92,0x14,0x10,
        0xc4,0xa2,0x75,0x92,0x04,0x20,
        0xc4,0xc2,0x75,0x92,0x04,0x11,
        0xc4,0xe2,0x1d,0x92,0x04,0x10,
        0xc4,0xe2,0x1d,0x92,0x04,0xd0,
    });

    test_asm(r, [&](A& a) {
        a.movq(A::rax, A::rdi, 0);
        a.movq(A::rax, A::rdi, 1);
        a.movq(A::rax, A::rdi, 512);
        a.movq(A::r15, A::r13, 42);
        a.movq(A::rax, A::r13, 42);
        a.movq(A::r15, A::rax, 42);
    },{
        0x48, 0x8b, 0x07,
        0x48, 0x8b, 0x47, 0x01,
        0x48, 0x8b, 0x87, 0x00,0x02,0x00,0x00,
        0x4d, 0x8b, 0x7d, 0x2a,
        0x49, 0x8b, 0x45, 0x2a,
        0x4c, 0x8b, 0x78, 0x2a,
    });

    // echo "fmul v4.4s, v3.4s, v1.4s" | llvm-mc -show-encoding -arch arm64

    test_asm(r, [&](A& a) {
        a.and16b(A::v4, A::v3, A::v1);
        a.orr16b(A::v4, A::v3, A::v1);
        a.eor16b(A::v4, A::v3, A::v1);
        a.bic16b(A::v4, A::v3, A::v1);
        a.bsl16b(A::v4, A::v3, A::v1);
        a.not16b(A::v4, A::v3);

        a.add4s(A::v4, A::v3, A::v1);
        a.sub4s(A::v4, A::v3, A::v1);
        a.mul4s(A::v4, A::v3, A::v1);

        a.cmeq4s(A::v4, A::v3, A::v1);
        a.cmgt4s(A::v4, A::v3, A::v1);

        a.sub8h(A::v4, A::v3, A::v1);
        a.mul8h(A::v4, A::v3, A::v1);

        a.fadd4s(A::v4, A::v3, A::v1);
        a.fsub4s(A::v4, A::v3, A::v1);
        a.fmul4s(A::v4, A::v3, A::v1);
        a.fdiv4s(A::v4, A::v3, A::v1);
        a.fmin4s(A::v4, A::v3, A::v1);
        a.fmax4s(A::v4, A::v3, A::v1);
        a.fneg4s(A::v4, A::v3);

        a.fmla4s(A::v4, A::v3, A::v1);
        a.fmls4s(A::v4, A::v3, A::v1);

        a.fcmeq4s(A::v4, A::v3, A::v1);
        a.fcmgt4s(A::v4, A::v3, A::v1);
        a.fcmge4s(A::v4, A::v3, A::v1);
    },{
        0x64,0x1c,0x21,0x4e,
        0x64,0x1c,0xa1,0x4e,
        0x64,0x1c,0x21,0x6e,
        0x64,0x1c,0x61,0x4e,
        0x64,0x1c,0x61,0x6e,
        0x64,0x58,0x20,0x6e,

        0x64,0x84,0xa1,0x4e,
        0x64,0x84,0xa1,0x6e,
        0x64,0x9c,0xa1,0x4e,

        0x64,0x8c,0xa1,0x6e,
        0x64,0x34,0xa1,0x4e,

        0x64,0x84,0x61,0x6e,
        0x64,0x9c,0x61,0x4e,

        0x64,0xd4,0x21,0x4e,
        0x64,0xd4,0xa1,0x4e,
        0x64,0xdc,0x21,0x6e,
        0x64,0xfc,0x21,0x6e,
        0x64,0xf4,0xa1,0x4e,
        0x64,0xf4,0x21,0x4e,
        0x64,0xf8,0xa0,0x6e,

        0x64,0xcc,0x21,0x4e,
        0x64,0xcc,0xa1,0x4e,

        0x64,0xe4,0x21,0x4e,
        0x64,0xe4,0xa1,0x6e,
        0x64,0xe4,0x21,0x6e,
    });

    test_asm(r, [&](A& a) {
        a.shl4s(A::v4, A::v3,  0);
        a.shl4s(A::v4, A::v3,  1);
        a.shl4s(A::v4, A::v3,  8);
        a.shl4s(A::v4, A::v3, 16);
        a.shl4s(A::v4, A::v3, 31);

        a.sshr4s(A::v4, A::v3,  1);
        a.sshr4s(A::v4, A::v3,  8);
        a.sshr4s(A::v4, A::v3, 31);

        a.ushr4s(A::v4, A::v3,  1);
        a.ushr4s(A::v4, A::v3,  8);
        a.ushr4s(A::v4, A::v3, 31);

        a.ushr8h(A::v4, A::v3,  1);
        a.ushr8h(A::v4, A::v3,  8);
        a.ushr8h(A::v4, A::v3, 15);
    },{
        0x64,0x54,0x20,0x4f,
        0x64,0x54,0x21,0x4f,
        0x64,0x54,0x28,0x4f,
        0x64,0x54,0x30,0x4f,
        0x64,0x54,0x3f,0x4f,

        0x64,0x04,0x3f,0x4f,
        0x64,0x04,0x38,0x4f,
        0x64,0x04,0x21,0x4f,

        0x64,0x04,0x3f,0x6f,
        0x64,0x04,0x38,0x6f,
        0x64,0x04,0x21,0x6f,

        0x64,0x04,0x1f,0x6f,
        0x64,0x04,0x18,0x6f,
        0x64,0x04,0x11,0x6f,
    });

    test_asm(r, [&](A& a) {
        a.sli4s(A::v4, A::v3,  0);
        a.sli4s(A::v4, A::v3,  1);
        a.sli4s(A::v4, A::v3,  8);
        a.sli4s(A::v4, A::v3, 16);
        a.sli4s(A::v4, A::v3, 31);
    },{
        0x64,0x54,0x20,0x6f,
        0x64,0x54,0x21,0x6f,
        0x64,0x54,0x28,0x6f,
        0x64,0x54,0x30,0x6f,
        0x64,0x54,0x3f,0x6f,
    });

    test_asm(r, [&](A& a) {
        a.scvtf4s (A::v4, A::v3);
        a.fcvtzs4s(A::v4, A::v3);
        a.fcvtns4s(A::v4, A::v3);
    },{
        0x64,0xd8,0x21,0x4e,
        0x64,0xb8,0xa1,0x4e,
        0x64,0xa8,0x21,0x4e,
    });

    test_asm(r, [&](A& a) {
        a.brk(0);
        a.brk(65535);

        a.ret(A::x30);   // Conventional ret using link register.
        a.ret(A::x13);   // Can really return using any register if we like.

        a.add(A::x2, A::x2,  4);
        a.add(A::x3, A::x2, 32);

        a.sub(A::x2, A::x2, 4);
        a.sub(A::x3, A::x2, 32);

        a.subs(A::x2, A::x2,  4);
        a.subs(A::x3, A::x2, 32);

        a.subs(A::xzr, A::x2, 4);  // These are actually the same instruction!
        a.cmp(A::x2, 4);

        A::Label l = a.here();
        a.bne(&l);
        a.bne(&l);
        a.blt(&l);
        a.b(&l);
        a.cbnz(A::x2, &l);
        a.cbz(A::x2, &l);
    },{
        0x00,0x00,0x20,0xd4,
        0xe0,0xff,0x3f,0xd4,

        0xc0,0x03,0x5f,0xd6,
        0xa0,0x01,0x5f,0xd6,

        0x42,0x10,0x00,0x91,
        0x43,0x80,0x00,0x91,

        0x42,0x10,0x00,0xd1,
        0x43,0x80,0x00,0xd1,

        0x42,0x10,0x00,0xf1,
        0x43,0x80,0x00,0xf1,

        0x5f,0x10,0x00,0xf1,
        0x5f,0x10,0x00,0xf1,

        0x01,0x00,0x00,0x54,   // b.ne #0
        0xe1,0xff,0xff,0x54,   // b.ne #-4
        0xcb,0xff,0xff,0x54,   // b.lt #-8
        0xae,0xff,0xff,0x54,   // b.al #-12
        0x82,0xff,0xff,0xb5,   // cbnz x2, #-16
        0x62,0xff,0xff,0xb4,   // cbz x2, #-20
    });

    // Can we cbz() to a not-yet-defined label?
    test_asm(r, [&](A& a) {
        A::Label l;
        a.cbz(A::x2, &l);
        a.add(A::x3, A::x2, 32);
        a.label(&l);
        a.ret(A::x30);
    },{
        0x42,0x00,0x00,0xb4,  // cbz x2, #8
        0x43,0x80,0x00,0x91,  // add x3, x2, #32
        0xc0,0x03,0x5f,0xd6,  // ret
    });

    // If we start a label as a backward label,
    // can we redefine it to be a future label?
    // (Not sure this is useful... just want to test it works.)
    test_asm(r, [&](A& a) {
        A::Label l1 = a.here();
        a.add(A::x3, A::x2, 32);
        a.cbz(A::x2, &l1);          // This will jump backward... nothing sneaky.

        A::Label l2 = a.here();     // Start off the same...
        a.add(A::x3, A::x2, 32);
        a.cbz(A::x2, &l2);          // Looks like this will go backward...
        a.add(A::x2, A::x2, 4);
        a.add(A::x3, A::x2, 32);
        a.label(&l2);               // But no... actually forward!  What a switcheroo!
    },{
        0x43,0x80,0x00,0x91,  // add x3, x2, #32
        0xe2,0xff,0xff,0xb4,  // cbz x2, #-4

        0x43,0x80,0x00,0x91,  // add x3, x2, #32
        0x62,0x00,0x00,0xb4,  // cbz x2, #12
        0x42,0x10,0x00,0x91,  // add x2, x2, #4
        0x43,0x80,0x00,0x91,  // add x3, x2, #32
    });

    // Loading from a label on ARM.
    test_asm(r, [&](A& a) {
        A::Label fore,aft;
        a.label(&fore);
        a.word(0x01234567);
        a.ldrq(A::v1, &fore);
        a.ldrq(A::v2, &aft);
        a.label(&aft);
        a.word(0x76543210);
    },{
        0x67,0x45,0x23,0x01,
        0xe1,0xff,0xff,0x9c,  // ldr q1, #-4
        0x22,0x00,0x00,0x9c,  // ldr q2, #4
        0x10,0x32,0x54,0x76,
    });

    test_asm(r, [&](A& a) {
        a.ldrq(A::v0, A::x8);
        a.strq(A::v0, A::x8);
    },{
        0x00,0x01,0xc0,0x3d,
        0x00,0x01,0x80,0x3d,
    });

    test_asm(r, [&](A& a) {
        a.xtns2h(A::v0, A::v0);
        a.xtnh2b(A::v0, A::v0);
        a.strs  (A::v0, A::x0);

        a.ldrs   (A::v0, A::x0);
        a.uxtlb2h(A::v0, A::v0);
        a.uxtlh2s(A::v0, A::v0);

        a.uminv4s(A::v3, A::v4);
        a.fmovs  (A::x3, A::v4);  // fmov w3,s4
    },{
        0x00,0x28,0x61,0x0e,
        0x00,0x28,0x21,0x0e,
        0x00,0x00,0x00,0xbd,

        0x00,0x00,0x40,0xbd,
        0x00,0xa4,0x08,0x2f,
        0x00,0xa4,0x10,0x2f,

        0x83,0xa8,0xb1,0x6e,
        0x83,0x00,0x26,0x1e,
    });

    test_asm(r, [&](A& a) {
        a.ldrb(A::v0, A::x8);
        a.strb(A::v0, A::x8);
    },{
        0x00,0x01,0x40,0x3d,
        0x00,0x01,0x00,0x3d,
    });

    test_asm(r, [&](A& a) {
        a.tbl(A::v0, A::v1, A::v2);
    },{
        0x20,0x00,0x02,0x4e,
    });
}

DEF_TEST(SkVM_approx_math, r) {
    auto eval = [](int N, float values[], auto fn) {
        skvm::Builder b;
        skvm::Arg inout  = b.varying<float>();

        b.storeF(inout, fn(&b, b.loadF(inout)));

        b.done().eval(N, values);
    };

    auto compare = [r](int N, const float values[], const float expected[]) {
        for (int i = 0; i < N; ++i) {
            REPORTER_ASSERT(r, SkScalarNearlyEqual(values[i], expected[i], 0.001f));
        }
    };

    // log2
    {
        float values[] = {0.25f, 0.5f, 1, 2, 4, 8};
        constexpr int N = SK_ARRAY_COUNT(values);
        eval(N, values, [](skvm::Builder* b, skvm::F32 v) {
            return b->approx_log2(v);
        });
        const float expected[] = {-2, -1, 0, 1, 2, 3};
        compare(N, values, expected);
    }

    // pow2
    {
        float values[] = {-2, -1, 0, 1, 2, 3};
        constexpr int N = SK_ARRAY_COUNT(values);
        eval(N, values, [](skvm::Builder* b, skvm::F32 v) {
            return b->approx_pow2(v);
        });
        const float expected[] = {0.25f, 0.5f, 1, 2, 4, 8};
        compare(N, values, expected);
    }

    // powf -- x^0.5
    {
        float bases[] = {0, 1, 4, 9, 16};
        constexpr int N = SK_ARRAY_COUNT(bases);
        eval(N, bases, [](skvm::Builder* b, skvm::F32 base) {
            return b->approx_powf(base, b->splat(0.5f));
        });
        const float expected[] = {0, 1, 2, 3, 4};
        compare(N, bases, expected);
    }
    // powf -- 3^x
    {
        float exps[] = {-2, -1, 0, 1, 2};
        constexpr int N = SK_ARRAY_COUNT(exps);
        eval(N, exps, [](skvm::Builder* b, skvm::F32 exp) {
            return b->approx_powf(b->splat(3.0f), exp);
        });
        const float expected[] = {1/9.0f, 1/3.0f, 1, 3, 9};
        compare(N, exps, expected);
    }
}
