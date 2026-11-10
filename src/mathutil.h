#pragma once
#include <cmath>

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() {}
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float Length(const Vec3& a) { return sqrtf(Dot(a, a)); }
inline Vec3 Normalize(const Vec3& a) {
    float l = Length(a);
    return l > 1e-8f ? a * (1.0f / l) : Vec3(1, 0, 0);
}

// Row-major 3x4 rigid transform, same layout as OpenVR's HmdMatrix34_t.
struct Mat34 {
    float m[3][4];
    static Mat34 Identity() {
        Mat34 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) r.m[i][j] = (i == j) ? 1.0f : 0.0f;
        return r;
    }
    Vec3 Col(int c) const { return Vec3(m[0][c], m[1][c], m[2][c]); }
    Vec3 Pos() const { return Col(3); }
    Vec3 Right() const { return Col(0); }
    Vec3 Up() const { return Col(1); }
    Vec3 Forward() const { return Col(2) * -1.0f; }
};

inline Mat34 Mul(const Mat34& a, const Mat34& b) {
    Mat34 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float v = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) v += a.m[i][3];
            r.m[i][j] = v;
        }
    }
    return r;
}

// Rotation about the local X axis (pitch), degrees. Positive tilts forward (-Z) up.
inline Mat34 RotX(float deg) {
    float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    Mat34 r = Mat34::Identity();
    r.m[1][1] = c; r.m[1][2] = -s;
    r.m[2][1] = s; r.m[2][2] = c;
    return r;
}

inline Mat34 Translate(float x, float y, float z) {
    Mat34 r = Mat34::Identity();
    r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
    return r;
}

// Unreal rotator in 65536-per-turn units.
struct Rotator {
    int pitch = 0, yaw = 0, roll = 0;
};

inline int RadToUnreal(float r) { return (int)lroundf(r * (32768.0f / 3.14159265f)); }

// Builds an Unreal rotator from orthonormal forward/right/up vectors given in
// Unreal axes (X forward, Y right, Z up).
inline Rotator RotatorFromAxes(const Vec3& f, const Vec3& r, const Vec3& u) {
    Rotator out;
    out.yaw = RadToUnreal(atan2f(f.y, f.x));
    out.pitch = RadToUnreal(atan2f(f.z, sqrtf(f.x * f.x + f.y * f.y)));
    out.roll = RadToUnreal(atan2f(-r.z, u.z));
    return out;
}
