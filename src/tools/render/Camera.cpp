// PPet - C++ 移植版
// 相机实现。
#include <src/tools/render/Camera.hpp>

#include <algorithm>
#include <cmath>

namespace ppet {
namespace {

/// 构造右手系下的 look-at 视图矩阵（相机看向自己的 -Z，up 为 +Y）。
/// 与 PMX / VRM 后端各自内部的同名函数保持一致的实现，避免同一场景出现两套朝向约定。
Mat4 BuildLookAtMatrix(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 backward = Normalize(eye - target);
    Vec3 right = Cross(up, backward);
    if (Length(right) < kEpsilon) {
        // up 与视线共线时右向量无定义，退化为固定轴以避免 NaN 污染整个矩阵。
        right = Vec3(1.0f, 0.0f, 0.0f);
    } else {
        right = Normalize(right);
    }
    const Vec3 trueUp = Cross(backward, right);

    Mat4 view;
    view.At(0, 0) = right.x;    view.At(0, 1) = right.y;    view.At(0, 2) = right.z;
    view.At(1, 0) = trueUp.x;   view.At(1, 1) = trueUp.y;   view.At(1, 2) = trueUp.z;
    view.At(2, 0) = backward.x; view.At(2, 1) = backward.y; view.At(2, 2) = backward.z;
    view.At(0, 3) = -Dot(right, eye);
    view.At(1, 3) = -Dot(trueUp, eye);
    view.At(2, 3) = -Dot(backward, eye);
    view.At(3, 3) = 1.0f;
    return view;
}

/// 取模型包围盒；若 mesh 上没有有效的包围盒信息，则退回直接扫描顶点。
struct Bounds {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};
    bool valid = false;
};

Bounds ComputeMeshBounds(const SkinnedMesh& mesh) {
    Bounds bounds;
    // mesh.boundsMin/Max 由 ComputeBounds() 填充；这里额外做一次有效性判断，
    // 因为调用方可能还没跑过 ComputeBounds（此时包围盒是全零的退化值）。
    if (mesh.boundsMax.x > mesh.boundsMin.x || mesh.boundsMax.y > mesh.boundsMin.y ||
        mesh.boundsMax.z > mesh.boundsMin.z) {
        bounds.min = mesh.boundsMin;
        bounds.max = mesh.boundsMax;
        bounds.valid = true;
        return bounds;
    }
    if (mesh.vertices.empty()) return bounds;

    Vec3 lo = mesh.vertices.front().position;
    Vec3 hi = lo;
    for (const Vertex& v : mesh.vertices) {
        lo.x = std::min(lo.x, v.position.x);
        lo.y = std::min(lo.y, v.position.y);
        lo.z = std::min(lo.z, v.position.z);
        hi.x = std::max(hi.x, v.position.x);
        hi.y = std::max(hi.y, v.position.y);
        hi.z = std::max(hi.z, v.position.z);
    }
    bounds.min = lo;
    bounds.max = hi;
    bounds.valid = true;
    return bounds;
}

}  // namespace

Camera Camera::FitOrthographic(const SkinnedMesh& mesh, float aspect, float padding) {
    Camera camera;

    const Bounds bounds = ComputeMeshBounds(mesh);
    const Vec3 center = bounds.valid
                            ? (bounds.min + bounds.max) * 0.5f
                            : Vec3(0.0f, 0.0f, 0.0f);

    float modelWidth = 0.0f;
    float modelHeight = 0.0f;
    if (bounds.valid) {
        modelWidth = bounds.max.x - bounds.min.x;
        modelHeight = bounds.max.y - bounds.min.y;
    } else if (mesh.modelHeight > 0.0f) {
        modelHeight = mesh.modelHeight;
        modelWidth = modelHeight;
    }
    modelWidth = std::max(modelWidth, kEpsilon);
    modelHeight = std::max(modelHeight, kEpsilon);

    const float safeAspect = std::max(aspect, kEpsilon);
    // 纵向半高至少要装下模型高度；模型比视口更宽时，反过来由宽度决定半高，
    // 这样无论什么宽高比都不会把模型裁掉。
    float halfHeight = std::max(modelHeight * 0.5f, (modelWidth * 0.5f) / safeAspect);
    halfHeight *= std::max(padding, 1.0f);

    // 相机放在模型正前方（+Z），看向模型中心：
    // PMX 与 VRM 的正面都朝 +Z，这样取到的就是正面而不是后脑勺。
    const float distance = std::max(modelHeight, modelWidth) * 4.0f;
    const Vec3 eye(center.x, center.y, center.z + distance);

    const float nearZ = std::max(distance - halfHeight * 2.0f - modelHeight, 0.01f);
    const float farZ = distance + halfHeight * 2.0f + modelHeight;

    camera.projection = Ortho(-halfHeight * safeAspect, halfHeight * safeAspect, -halfHeight,
                               halfHeight, nearZ, farZ);
    camera.LookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
    return camera;
}

Camera Camera::FitPerspective(const SkinnedMesh& mesh, float aspect, float fovY) {
    Camera camera;

    const Bounds bounds = ComputeMeshBounds(mesh);
    const Vec3 center = bounds.valid
                            ? (bounds.min + bounds.max) * 0.5f
                            : Vec3(0.0f, 0.0f, 0.0f);

    float modelWidth = 0.0f;
    float modelHeight = 0.0f;
    if (bounds.valid) {
        modelWidth = bounds.max.x - bounds.min.x;
        modelHeight = bounds.max.y - bounds.min.y;
    } else if (mesh.modelHeight > 0.0f) {
        modelHeight = mesh.modelHeight;
        modelWidth = modelHeight;
    }
    modelWidth = std::max(modelWidth, kEpsilon);
    modelHeight = std::max(modelHeight, kEpsilon);

    const float safeAspect = std::max(aspect, kEpsilon);
    // fovY 以「度」为单位，这里转成弧度传给 Perspective()。
    const float fovRad = std::max(fovY, 1.0f) * kPi / 180.0f;

    // 让模型在竖直方向上刚好填满视锥：需要的距离 = 半高 / tan(fov/2)。
    // 再乘上安全系数，保证模型稍微转向时也不会被裁掉。
    const float halfHeight = std::max(modelHeight * 0.5f, (modelWidth * 0.5f) / safeAspect);
    float distance = halfHeight / std::tan(fovRad * 0.5f);
    distance *= 1.2f;

    const Vec3 eye(center.x, center.y, center.z + distance);
    const float nearZ = std::max(distance - halfHeight * 2.0f, 0.01f);
    const float farZ = distance + halfHeight * 4.0f + modelHeight * 2.0f;

    camera.projection = Perspective(fovRad, safeAspect, nearZ, farZ);
    camera.LookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
    return camera;
}

void Camera::LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    this->position = eye;
    this->target = target;
    view = BuildLookAtMatrix(eye, target, up);
    // 列主序下矩阵乘法顺序与直觉一致：先 view 后 projection。
    viewProjection = projection * view;
}

}  // namespace ppet
