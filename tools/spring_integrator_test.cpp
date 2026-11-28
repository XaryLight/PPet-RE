// VRM 弹簧骨积分器单测：验证「自然物理效果」的关键契约。
#include <src/tools/core/Math.hpp>
#include <src/tools/model/SpringIntegrator.hpp>

#include <cmath>
#include <cstdio>

using namespace ppet;

static int g_asserts = 0;
static int g_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_asserts;                                                           \
        if (!(cond)) {                                                         \
            ++g_failed;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

// 1. 无外力静止：current==previous，无回正、无重力，应停在原地（不会凭空摆动）。
static void TestRestStable() {
    std::printf("-- 静止稳定 --\n");
    SpringIntegratorParams p;
    p.stiffness = 0.0f;
    p.dragForce = 0.4f;
    p.gravityPower = 0.0f;
    p.restDirection = Vec3(0.0f, 1.0f, 0.0f);
    p.anchor = Vec3(0.0f, 0.0f, 0.0f);
    p.restLength = -1.0f;  // 不施加骨长约束
    const Vec3 rest(0.0f, 1.0f, 0.0f);
    const Vec3 next = SpringIntegrate(rest, rest, p, 1.0f / 60.0f);
    CHECK(Near(next.x, 0.0f) && Near(next.y, 1.0f) && Near(next.z, 0.0f));
}

// 2. stiffness 回正力：骨骼被拉离静止方向后，应朝 restDirection 方向被拉回。
//    这是旧实现完全缺失的力——没有它骨骼垂下后永不回摆。
static void TestStiffnessRestores() {
    std::printf("-- stiffness 回正力 --\n");
    SpringIntegratorParams p;
    p.stiffness = 10.0f;   // 强回正，便于观察
    p.dragForce = 0.5f;
    p.gravityPower = 0.0f;
    p.restDirection = Vec3(0.0f, 1.0f, 0.0f);
    p.restLength = -1.0f;
    const Vec3 displaced(0.5f, 1.0f, 0.0f);
    const Vec3 next = SpringIntegrate(displaced, displaced, p, 0.1f);
    // 回正力沿 +Y（restDirection），x 分量应保持（惯性为 0），y 应增大。
    CHECK(Near(next.x, 0.5f));
    CHECK(next.y > displaced.y);
}

// 3. 惯性方向：dragForce 越小，摆动保持越久。旧实现乘 dragForce 会反向。
static void TestInertiaDirection() {
    std::printf("-- 惯性=1-dragForce --\n");
    SpringIntegratorParams p;
    p.stiffness = 0.0f;
    p.dragForce = 0.1f;  // 低阻尼 -> 保留 90% 惯性
    p.gravityPower = 0.0f;
    p.restLength = -1.0f;
    const Vec3 current(0.0f, 0.0f, 0.0f);
    const Vec3 previous(1.0f, 0.0f, 0.0f);  // 上一帧在 +X，即正在朝 -X 移动
    const Vec3 next = SpringIntegrate(current, previous, p, 1.0f / 60.0f);
    // inertia = (current - previous) * (1 - drag) = (-X) * 0.9 -> x 应为负。
    CHECK(next.x < -0.5f);
    CHECK(Near(next.x, -0.9f));
}

// 4. 重力量级：重力项应为 gravityPower * dt（不是 dt²）。
static void TestGravityUsesDt() {
    std::printf("-- 重力=dt 而非 dt² --\n");
    SpringIntegratorParams p;
    p.stiffness = 0.0f;
    p.dragForce = 1.0f;  // 完全消除惯性
    p.gravityPower = 1.0f;
    p.gravityDir = Vec3(0.0f, -1.0f, 0.0f);
    p.restLength = -1.0f;
    const Vec3 current(0.0f, 0.0f, 0.0f);
    const float dt = 0.1f;
    const Vec3 next = SpringIntegrate(current, current, p, dt);
    // 期望 y = -gravityPower * dt = -0.1；旧 dt² 实现只有 -0.01。
    CHECK(Near(next.y, -0.1f));
}

// 5. 骨长约束：末端不能被拉离锚点超过 restLength。
static void TestLengthConstraint() {
    std::printf("-- 骨长约束 --\n");
    SpringIntegratorParams p;
    p.stiffness = 0.0f;
    p.dragForce = 0.0f;
    p.gravityPower = 0.0f;
    p.anchor = Vec3(0.0f, 0.0f, 0.0f);
    p.restLength = 1.0f;
    const Vec3 current(0.0f, 5.0f, 0.0f);  // 远超骨长
    const Vec3 next = SpringIntegrate(current, current, p, 1.0f / 60.0f);
    CHECK(Near(Length(next), 1.0f));
}

// 6. NaN/Inf 防御：非有限输入不应污染输出。
static void TestNonFiniteGuard() {
    std::printf("-- NaN 防御 --\n");
    SpringIntegratorParams p;
    p.stiffness = 1.0f;
    p.restLength = -1.0f;
    const float inf = std::numeric_limits<float>::infinity();
    const Vec3 bad(inf, 0.0f, 0.0f);
    const Vec3 current(0.0f, 1.0f, 0.0f);
    const Vec3 next = SpringIntegrate(bad, current, p, 1.0f / 60.0f);
    CHECK(std::isfinite(next.x) && std::isfinite(next.y) && std::isfinite(next.z));
}

int main() {
    std::printf("=== VRM 弹簧骨积分器单测 ===\n");
    TestRestStable();
    TestStiffnessRestores();
    TestInertiaDirection();
    TestGravityUsesDt();
    TestLengthConstraint();
    TestNonFiniteGuard();
    std::printf("断言 %d，失败 %d\n", g_asserts, g_failed);
    return g_failed == 0 ? 0 : 1;
}
