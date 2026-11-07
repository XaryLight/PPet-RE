// PPet - C++ 移植版
// 基础数学类型与工具函数。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ppet {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kEpsilon = 1e-6f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
};

inline float Length(const Vec2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

/// 整数矩形：窗口 / 虚拟桌面的几何，坐标可为负（多显示器时左侧屏幕的 x 为负）。
struct IntRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// 把窗口矩形整体钳制进虚拟桌面，保证窗口始终可见。
///
/// 为什么需要：透明置顶无边框窗口一旦被拖到桌面外就没有任何可点的地方，
/// 而配置里保存的坐标会让下次启动继续不可见，形成死循环。所有程序化设置位置
/// 的地方（加载配置 / 托盘重置 / 结束拖动）都先过这里。
///
/// 规则：
///   - 桌面尺寸无效时原样返回，避免无显示器信息的环境把窗口强行挤到 (0,0)；
///   - 窗口比桌面还大时退回桌面左上角，至少保证左上角可见；
///   - 否则把左上角限制在 [desktop.x, desktop.right - window.width] 内。
inline IntRect ClampRectInside(const IntRect& window, const IntRect& desktop) {
    if (desktop.width <= 0 || desktop.height <= 0) return window;

    const int maxX = desktop.x + (desktop.width > window.width
                                      ? desktop.width - window.width
                                      : 0);
    const int maxY = desktop.y + (desktop.height > window.height
                                      ? desktop.height - window.height
                                      : 0);
    IntRect out = window;
    out.x = std::clamp(window.x, desktop.x, std::max(desktop.x, maxX));
    out.y = std::clamp(window.y, desktop.y, std::max(desktop.y, maxY));
    return out;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Normalize(const Vec3& v) {
    const float len = Length(v);
    if (len < kEpsilon) return {0.0f, 0.0f, 0.0f};
    return v / len;
}

inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

/// 4x4 矩阵，按列主序存储（与 OpenGL 一致）。
/// m[col * 4 + row]，第 4 列为平移。
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float& At(int row, int col) { return m[col * 4 + row]; }
    float At(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 Identity() { return Mat4{}; }

    static Mat4 Translation(const Vec3& t) {
        Mat4 r;
        r.At(0, 3) = t.x;
        r.At(1, 3) = t.y;
        r.At(2, 3) = t.z;
        return r;
    }

    static Mat4 Scale(const Vec3& s) {
        Mat4 r;
        r.At(0, 0) = s.x;
        r.At(1, 1) = s.y;
        r.At(2, 2) = s.z;
        return r;
    }

    static Mat4 RotationX(float rad) {
        Mat4 r;
        const float c = std::cos(rad), s = std::sin(rad);
        r.At(1, 1) = c; r.At(1, 2) = -s;
        r.At(2, 1) = s; r.At(2, 2) = c;
        return r;
    }

    static Mat4 RotationY(float rad) {
        Mat4 r;
        const float c = std::cos(rad), s = std::sin(rad);
        r.At(0, 0) = c; r.At(0, 2) = s;
        r.At(2, 0) = -s; r.At(2, 2) = c;
        return r;
    }

    static Mat4 RotationZ(float rad) {
        Mat4 r;
        const float c = std::cos(rad), s = std::sin(rad);
        r.At(0, 0) = c; r.At(0, 1) = -s;
        r.At(1, 0) = s; r.At(1, 1) = c;
        return r;
    }

    /// 欧拉角转旋转矩阵（XYZ 顺序），用于骨骼初始姿态。
    static Mat4 FromEuler(const Vec3& rad) {
        return RotationZ(rad.z) * RotationY(rad.y) * RotationX(rad.x);
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += At(row, k) * o.At(k, c);
                }
                r.At(row, c) = sum;
            }
        }
        return r;
    }

    Vec3 TransformPoint(const Vec3& p) const {
        const float x = At(0, 0) * p.x + At(0, 1) * p.y + At(0, 2) * p.z + At(0, 3);
        const float y = At(1, 0) * p.x + At(1, 1) * p.y + At(1, 2) * p.z + At(1, 3);
        const float z = At(2, 0) * p.x + At(2, 1) * p.y + At(2, 2) * p.z + At(2, 3);
        const float w = At(3, 0) * p.x + At(3, 1) * p.y + At(3, 2) * p.z + At(3, 3);
        if (std::fabs(w) > kEpsilon && std::fabs(w - 1.0f) > kEpsilon) {
            return {x / w, y / w, z / w};
        }
        return {x, y, z};
    }

    Vec3 TransformDirection(const Vec3& d) const {
        return {At(0, 0) * d.x + At(0, 1) * d.y + At(0, 2) * d.z,
                At(1, 0) * d.x + At(1, 1) * d.y + At(1, 2) * d.z,
                At(2, 0) * d.x + At(2, 1) * d.y + At(2, 2) * d.z};
    }

    Mat4 Transpose() const {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) r.At(row, c) = At(c, row);
        }
        return r;
    }

    /// 逆矩阵（通用 4x4 高斯消元）。矩阵不可逆时返回单位阵。
    Mat4 Inverse() const {
        double a[4][8];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) a[r][c] = At(r, c);
            for (int c = 0; c < 4; ++c) a[r][4 + c] = (r == c) ? 1.0 : 0.0;
        }
        for (int i = 0; i < 4; ++i) {
            int pivot = i;
            for (int r = i + 1; r < 4; ++r) {
                if (std::fabs(a[r][i]) > std::fabs(a[pivot][i])) pivot = r;
            }
            if (std::fabs(a[pivot][i]) < 1e-12) return Mat4::Identity();
            if (pivot != i) {
                for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[pivot][c]);
            }
            const double inv = 1.0 / a[i][i];
            for (int c = 0; c < 8; ++c) a[i][c] *= inv;
            for (int r = 0; r < 4; ++r) {
                if (r == i) continue;
                const double f = a[r][i];
                if (f == 0.0) continue;
                for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c];
            }
        }
        Mat4 out;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) out.At(r, c) = static_cast<float>(a[r][4 + c]);
        }
        return out;
    }
};

/// 透视投影矩阵（右手系，深度范围 -1..1）。
inline Mat4 Perspective(float fovYRad, float aspect, float nearZ, float farZ) {
    Mat4 r;
    for (float& v : r.m) v = 0.0f;
    const float f = 1.0f / std::tan(fovYRad * 0.5f);
    r.At(0, 0) = f / aspect;
    r.At(1, 1) = f;
    r.At(2, 2) = (farZ + nearZ) / (nearZ - farZ);
    r.At(2, 3) = (2.0f * farZ * nearZ) / (nearZ - farZ);
    r.At(3, 2) = -1.0f;
    return r;
}

/// 正交投影矩阵。
inline Mat4 Ortho(float l, float r_, float b, float t, float n, float f) {
    Mat4 r;
    r.At(0, 0) = 2.0f / (r_ - l);
    r.At(1, 1) = 2.0f / (t - b);
    r.At(2, 2) = -2.0f / (f - n);
    r.At(0, 3) = -(r_ + l) / (r_ - l);
    r.At(1, 3) = -(t + b) / (t - b);
    r.At(2, 3) = -(f + n) / (f - n);
    return r;
}

/// 绕任意轴旋转向量（罗德里格公式），VRM/PMX 骨骼常用。
inline Vec3 RotateAroundAxis(const Vec3& v, const Vec3& axis, float angle) {
    const Vec3 n = Normalize(axis);
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(n, v) * s + n * (Dot(n, v) * (1.0f - c));
}

inline float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

/// 4 分量向量（RGBA 颜色 / 四元数分量）。
/// 说明：SkinnedMesh.hpp 中的 Material::diffuse 与 Vertex::color 使用该类型，
/// 因此必须与 Vec2/Vec3 定义在同一个命名空间中。
struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Vec4 operator/(float s) const { return {x / s, y / s, z / s, w / s}; }
    Vec4& operator+=(const Vec4& o) { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
};

inline Vec4 operator*(float s, const Vec4& v) { return v * s; }
inline float Dot(const Vec4& a, const Vec4& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

inline float SmoothStep(float edge0, float edge1, float x) {
    if (std::fabs(edge1 - edge0) < kEpsilon) return x < edge0 ? 0.0f : 1.0f;
    const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// 去除路径中的目录分隔符后的文件名。
inline std::string BaseName(const std::string& path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

}  // namespace ppet
