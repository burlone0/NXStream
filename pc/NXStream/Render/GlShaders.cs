namespace NXStream.Render;

internal static class GlShaders
{
    public const string VertexShader = """
        #version 330 core
        out vec2 v_pos;
        void main() {
            vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
            v_pos = p;
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        }
        """;

    public const string PresentShader = """
        #version 330 core
        in vec2 v_pos;
        out vec4 color;
        uniform sampler2D src;
        uniform vec2 src_size;
        uniform int mode;
        uniform float opacity;
        uniform vec2 dst_size;

        vec3 tap(vec2 texel) { return texture(src, (clamp(texel, vec2(0.0), src_size - 1.0) + 0.5) / src_size).rgb; }

        float hermite(float t) {
            t = abs(t);
            return t < 1.0 ? (2.0 * t - 3.0) * t * t + 1.0 : 0.0;
        }

        float luma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

        void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
            float lenX = max(abs(lD - lC), abs(lC - lB));
            float dirX = lD - lB;
            lenX = clamp(abs(dirX) / max(lenX, 1e-8), 0.0, 1.0);
            dir.x += dirX * w;
            len += lenX * lenX * w;
            float lenY = max(abs(lE - lC), abs(lC - lA));
            float dirY = lE - lA;
            lenY = clamp(abs(dirY) / max(lenY, 1e-8), 0.0, 1.0);
            dir.y += dirY * w;
            len += lenY * lenY * w;
        }

        void easuTap(inout vec3 acc, inout float accW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
            vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
            float d2 = min(v.x * v.x + v.y * v.y, clp);
            float wB = 2.0 / 5.0 * d2 - 1.0;
            float wA = lob * d2 - 1.0;
            wB *= wB;
            wA *= wA;
            wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
            float w = wB * wA;
            acc += c * w;
            accW += w;
        }

        vec3 easu(vec2 uv) {
            vec2 pp = uv * src_size - 0.5;
            vec2 fp = floor(pp);
            pp -= fp;
            vec3 b = tap(fp + vec2(0, -1)), c = tap(fp + vec2(1, -1));
            vec3 e = tap(fp + vec2(-1, 0)), f = tap(fp), g = tap(fp + vec2(1, 0)), h = tap(fp + vec2(2, 0));
            vec3 i = tap(fp + vec2(-1, 1)), j = tap(fp + vec2(0, 1)), k = tap(fp + vec2(1, 1)), l = tap(fp + vec2(2, 1));
            vec3 n = tap(fp + vec2(0, 2)), o = tap(fp + vec2(1, 2));
            float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
            float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);
            vec2 dir = vec2(0.0);
            float len = 0.0;
            easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
            easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
            easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
            easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
            float dirR = dir.x * dir.x + dir.y * dir.y;
            bool zero = dirR < 1.0 / 32768.0;
            dirR = zero ? 1.0 : inversesqrt(dirR);
            dir.x = zero ? 1.0 : dir.x;
            dir *= dirR;
            len = len * 0.5;
            len *= len;
            float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
            vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
            float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
            float clp = 1.0 / lob;
            vec3 mn = min(min(f, g), min(j, k));
            vec3 mx = max(max(f, g), max(j, k));
            vec3 acc = vec3(0.0);
            float accW = 0.0;
            easuTap(acc, accW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
            easuTap(acc, accW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
            easuTap(acc, accW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
            easuTap(acc, accW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
            easuTap(acc, accW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
            easuTap(acc, accW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
            easuTap(acc, accW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
            easuTap(acc, accW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
            easuTap(acc, accW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
            easuTap(acc, accW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
            easuTap(acc, accW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
            easuTap(acc, accW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
            return min(mx, max(mn, acc / accW));
        }

        void main() {
            vec2 uv = vec2(v_pos.x, 1.0 - v_pos.y);
            if (mode == 0) {
                color = vec4(texture(src, uv).rgb, opacity);
                return;
            }
            vec2 ratio = dst_size / src_size;
            if (ratio.x < 0.999 || ratio.y < 0.999) {
                vec2 r = min(ratio, vec2(1.0));
                vec2 p = uv * src_size - 0.5;
                vec2 base = floor(p);
                vec3 sum = vec3(0.0);
                float wsum = 0.0;
                for (int y = -2; y <= 2; y++)
                    for (int x = -2; x <= 2; x++) {
                        vec2 t = base + vec2(x, y);
                        vec2 d = (t - p) * r;
                        float w = hermite(d.x) * hermite(d.y);
                        sum += w * tap(t);
                        wsum += w;
                    }
                color = vec4(sum / wsum, opacity);
                return;
            }
            if (mode == 2) {
                color = vec4(easu(uv), opacity);
                return;
            }
            vec2 p = uv * src_size - 0.5;
            vec2 i = floor(p);
            vec2 f = p - i;
            vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
            vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
            vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
            vec2 w3 = f * f * (-0.5 + 0.5 * f);
            vec4 wx = vec4(w0.x, w1.x, w2.x, w3.x);
            vec4 wy = vec4(w0.y, w1.y, w2.y, w3.y);
            vec3 c = vec3(0.0);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    c += wx[x] * wy[y] * tap(i + vec2(x - 1, y - 1));
            color = vec4(clamp(c, 0.0, 1.0), opacity);
        }
        """;

    public const string SharpenShader = """
        #version 330 core
        in vec2 v_pos;
        out vec4 color;
        uniform sampler2D src;
        uniform float sharpness;

        vec3 at(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0).rgb; }

        void main() {
            ivec2 p = ivec2(v_pos * vec2(textureSize(src, 0)));
            vec3 b = at(p + ivec2(0, -1)), d = at(p + ivec2(-1, 0)), e = at(p), f = at(p + ivec2(1, 0)), h = at(p + ivec2(0, 1));
            vec3 mn = min(min(b, d), min(f, h));
            vec3 mx = max(max(b, d), max(f, h));
            vec3 hitMin = min(mn, e) / max(4.0 * mx, vec3(1e-5));
            vec3 hitMax = (1.0 - max(mx, e)) / min(4.0 * mn - 4.0, vec3(-1e-5));
            vec3 lobes = max(-hitMin, hitMax);
            float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * sharpness;
            color = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);
        }
        """;
}
