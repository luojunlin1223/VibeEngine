/*
 * VibeEngine Unit Tests
 *
 * Tests core engine systems that don't require a GPU context.
 * Uses doctest single-header testing framework.
 */
#define DOCTEST_CONFIG_IMPLEMENT

#include <doctest.h>

// Engine headers
#include <VibeEngine/Core/UUID.h>
#include <VibeEngine/Core/Log.h>
#include <VibeEngine/Scene/Scene.h>
#include <VibeEngine/Scene/Entity.h>
#include <VibeEngine/Scene/Components.h>
#include <VibeEngine/Scene/SceneSerializer.h>
#include <VibeEngine/Renderer/ShaderLab.h>
#include <VibeEngine/Renderer/Material.h>
#include <VibeEngine/Renderer/Frustum.h>
#include <VibeEngine/Input/InputAction.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <set>
#include <cmath>
#include <string>
#include <memory>

// ═══════════════════════════════════════════════════════════════════════
// Custom main to initialize the logging system before tests run
// ═══════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    VE::Log::Init();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();
    return res;
}

// ═══════════════════════════════════════════════════════════════════════
// 1. UUID Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("UUID") {

TEST_CASE("Generated UUIDs are non-zero") {
    VE::UUID a;
    VE::UUID b;
    CHECK(static_cast<uint64_t>(a) != 0);
    CHECK(static_cast<uint64_t>(b) != 0);
}

TEST_CASE("Generated UUIDs are unique") {
    constexpr int COUNT = 1000;
    std::set<uint64_t> ids;
    for (int i = 0; i < COUNT; i++) {
        VE::UUID uuid;
        ids.insert(static_cast<uint64_t>(uuid));
    }
    CHECK(ids.size() == COUNT);
}

TEST_CASE("UUID constructed from value preserves it") {
    VE::UUID uuid(12345678ULL);
    CHECK(static_cast<uint64_t>(uuid) == 12345678ULL);
}

TEST_CASE("UUID equality and inequality") {
    VE::UUID a(100);
    VE::UUID b(100);
    VE::UUID c(200);
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("UUID can be used as hash key") {
    std::unordered_map<VE::UUID, int> map;
    VE::UUID id(42);
    map[id] = 7;
    CHECK(map[id] == 7);
}

} // TEST_SUITE("UUID")


// ═══════════════════════════════════════════════════════════════════════
// 2. ECS / Scene Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("ECS") {

TEST_CASE("Create scene and entity") {
    VE::Scene scene;
    auto entity = scene.CreateEntity("TestEntity");
    CHECK(entity.IsValid());
    CHECK(entity.HasComponent<VE::TagComponent>());
    CHECK(entity.HasComponent<VE::IDComponent>());
    CHECK(entity.HasComponent<VE::TransformComponent>());
    CHECK(entity.HasComponent<VE::RelationshipComponent>());
}

TEST_CASE("Entity tag name is set correctly") {
    VE::Scene scene;
    auto entity = scene.CreateEntity("Player");
    CHECK(entity.GetComponent<VE::TagComponent>().Tag == "Player");
}

TEST_CASE("Default entity name is auto-generated") {
    VE::Scene scene;
    auto e1 = scene.CreateEntity();
    auto e2 = scene.CreateEntity();
    // Default names should be unique (e.g. "GameObject_0", "GameObject_1")
    CHECK(e1.GetComponent<VE::TagComponent>().Tag != e2.GetComponent<VE::TagComponent>().Tag);
}

TEST_CASE("Add and get custom component") {
    VE::Scene scene;
    auto entity = scene.CreateEntity("LightEntity");

    CHECK_FALSE(entity.HasComponent<VE::DirectionalLightComponent>());
    auto& light = entity.AddComponent<VE::DirectionalLightComponent>();
    light.Direction = { 0.0f, -1.0f, 0.0f };
    light.Intensity = 2.5f;

    CHECK(entity.HasComponent<VE::DirectionalLightComponent>());
    auto& retrieved = entity.GetComponent<VE::DirectionalLightComponent>();
    CHECK(retrieved.Intensity == doctest::Approx(2.5f));
    CHECK(retrieved.Direction[1] == doctest::Approx(-1.0f));
}

TEST_CASE("Remove component") {
    VE::Scene scene;
    auto entity = scene.CreateEntity("Test");
    entity.AddComponent<VE::DirectionalLightComponent>();
    CHECK(entity.HasComponent<VE::DirectionalLightComponent>());

    entity.RemoveComponent<VE::DirectionalLightComponent>();
    CHECK_FALSE(entity.HasComponent<VE::DirectionalLightComponent>());
}

TEST_CASE("Destroy entity") {
    VE::Scene scene;
    auto entity = scene.CreateEntity("Temp");
    auto handle = entity.GetHandle();
    scene.DestroyEntity(entity);
    CHECK_FALSE(scene.GetRegistry().valid(handle));
}

TEST_CASE("Create entity with specific UUID") {
    VE::Scene scene;
    VE::UUID specificId(9999);
    auto entity = scene.CreateEntityWithUUID(specificId, "SpecificEntity");
    CHECK(static_cast<uint64_t>(entity.GetComponent<VE::IDComponent>().ID) == 9999);
}

TEST_CASE("Multiple entities are independent") {
    VE::Scene scene;
    auto e1 = scene.CreateEntity("A");
    auto e2 = scene.CreateEntity("B");

    auto& tc1 = e1.GetComponent<VE::TransformComponent>();
    tc1.Position = { 1.0f, 2.0f, 3.0f };

    auto& tc2 = e2.GetComponent<VE::TransformComponent>();
    CHECK(tc2.Position[0] == doctest::Approx(0.0f));
    CHECK(tc2.Position[1] == doctest::Approx(0.0f));
    CHECK(tc2.Position[2] == doctest::Approx(0.0f));
}

TEST_CASE("GetAllEntitiesWith queries correct entities") {
    VE::Scene scene;
    auto e1 = scene.CreateEntity("WithLight");
    e1.AddComponent<VE::DirectionalLightComponent>();

    auto e2 = scene.CreateEntity("NoLight");
    auto e3 = scene.CreateEntity("WithLight2");
    e3.AddComponent<VE::DirectionalLightComponent>();

    auto view = scene.GetAllEntitiesWith<VE::DirectionalLightComponent>();
    int count = 0;
    for (auto ent : view) {
        (void)ent;
        count++;
    }
    CHECK(count == 2);
}

} // TEST_SUITE("ECS")


// ═══════════════════════════════════════════════════════════════════════
// 3. Entity Hierarchy Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("Hierarchy") {

TEST_CASE("SetParent establishes parent-child relationship") {
    VE::Scene scene;
    auto parent = scene.CreateEntity("Parent");
    auto child = scene.CreateEntity("Child");

    scene.SetParent(child.GetHandle(), parent.GetHandle());

    auto& childRel = child.GetComponent<VE::RelationshipComponent>();
    CHECK(childRel.Parent == parent.GetHandle());

    auto& parentRel = parent.GetComponent<VE::RelationshipComponent>();
    CHECK(parentRel.Children.size() == 1);
    CHECK(parentRel.Children[0] == child.GetHandle());
}

TEST_CASE("RemoveParent clears relationship") {
    VE::Scene scene;
    auto parent = scene.CreateEntity("Parent");
    auto child = scene.CreateEntity("Child");

    scene.SetParent(child.GetHandle(), parent.GetHandle());
    scene.RemoveParent(child.GetHandle());

    auto& childRel = child.GetComponent<VE::RelationshipComponent>();
    CHECK(childRel.Parent == static_cast<entt::entity>(entt::null));

    auto& parentRel = parent.GetComponent<VE::RelationshipComponent>();
    CHECK(parentRel.Children.empty());
}

TEST_CASE("GetWorldTransform composes parent and child transforms") {
    VE::Scene scene;
    auto parent = scene.CreateEntity("Parent");
    auto child = scene.CreateEntity("Child");

    // Set parent at (10, 0, 0)
    auto& ptc = parent.GetComponent<VE::TransformComponent>();
    ptc.Position = { 10.0f, 0.0f, 0.0f };

    // Set child local at (5, 0, 0)
    auto& ctc = child.GetComponent<VE::TransformComponent>();
    ctc.Position = { 5.0f, 0.0f, 0.0f };

    scene.SetParent(child.GetHandle(), parent.GetHandle());

    // After parenting, the child's local position is adjusted to preserve
    // its old world position. So we need to re-set it.
    auto& ctcAfter = child.GetComponent<VE::TransformComponent>();
    ctcAfter.Position = { 5.0f, 0.0f, 0.0f };

    glm::mat4 worldTransform = scene.GetWorldTransform(child.GetHandle());
    // World position should be parent(10) + child(5) = 15
    CHECK(worldTransform[3][0] == doctest::Approx(15.0f));
    CHECK(worldTransform[3][1] == doctest::Approx(0.0f));
    CHECK(worldTransform[3][2] == doctest::Approx(0.0f));
}

TEST_CASE("Circular parenting is prevented") {
    VE::Scene scene;
    auto a = scene.CreateEntity("A");
    auto b = scene.CreateEntity("B");

    scene.SetParent(b.GetHandle(), a.GetHandle()); // B is child of A
    scene.SetParent(a.GetHandle(), b.GetHandle()); // Should be rejected (circular)

    // A should NOT have B as parent
    auto& aRel = a.GetComponent<VE::RelationshipComponent>();
    CHECK(aRel.Parent == static_cast<entt::entity>(entt::null));
}

TEST_CASE("Entity GetChildren returns correct children") {
    VE::Scene scene;
    auto parent = scene.CreateEntity("Parent");
    auto c1 = scene.CreateEntity("Child1");
    auto c2 = scene.CreateEntity("Child2");

    scene.SetParent(c1.GetHandle(), parent.GetHandle());
    scene.SetParent(c2.GetHandle(), parent.GetHandle());

    auto children = parent.GetChildren();
    CHECK(children.size() == 2);
}

} // TEST_SUITE("Hierarchy")


// ═══════════════════════════════════════════════════════════════════════
// 4. Scene Serialization Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("SceneSerialization") {

TEST_CASE("Serialize and deserialize scene preserves entities") {
    // Create a scene with entities
    auto srcScene = std::make_shared<VE::Scene>();
    auto e1 = srcScene->CreateEntity("Hero");
    auto& tc1 = e1.GetComponent<VE::TransformComponent>();
    tc1.Position = { 1.0f, 2.0f, 3.0f };
    tc1.Scale = { 2.0f, 2.0f, 2.0f };

    auto e2 = srcScene->CreateEntity("Villain");
    auto& tc2 = e2.GetComponent<VE::TransformComponent>();
    tc2.Position = { -5.0f, 0.0f, 10.0f };

    uint64_t heroId = static_cast<uint64_t>(e1.GetComponent<VE::IDComponent>().ID);

    // Serialize to string
    VE::SceneSerializer srcSerializer(srcScene);
    std::string yaml = srcSerializer.SerializeToString();

    CHECK_FALSE(yaml.empty());

    // Deserialize into a new scene
    auto dstScene = std::make_shared<VE::Scene>();
    VE::SceneSerializer dstSerializer(dstScene);
    bool ok = dstSerializer.DeserializeFromString(yaml);
    CHECK(ok);

    // Verify: find entity by UUID
    bool foundHero = false;
    auto view = dstScene->GetAllEntitiesWith<VE::IDComponent, VE::TagComponent, VE::TransformComponent>();
    for (auto ent : view) {
        auto& id = dstScene->GetRegistry().get<VE::IDComponent>(ent);
        auto& tag = dstScene->GetRegistry().get<VE::TagComponent>(ent);
        auto& tc = dstScene->GetRegistry().get<VE::TransformComponent>(ent);
        if (static_cast<uint64_t>(id.ID) == heroId) {
            foundHero = true;
            CHECK(tag.Tag == "Hero");
            CHECK(tc.Position[0] == doctest::Approx(1.0f));
            CHECK(tc.Position[1] == doctest::Approx(2.0f));
            CHECK(tc.Position[2] == doctest::Approx(3.0f));
            CHECK(tc.Scale[0] == doctest::Approx(2.0f));
        }
    }
    CHECK(foundHero);
}

TEST_CASE("Serialize and deserialize preserves DirectionalLightComponent") {
    auto srcScene = std::make_shared<VE::Scene>();
    auto entity = srcScene->CreateEntity("Sun");
    auto& light = entity.AddComponent<VE::DirectionalLightComponent>();
    light.Direction = { 0.0f, -1.0f, 0.0f };
    light.Color = { 1.0f, 0.9f, 0.8f };
    light.Intensity = 1.5f;

    VE::SceneSerializer srcSerializer(srcScene);
    std::string yaml = srcSerializer.SerializeToString();

    auto dstScene = std::make_shared<VE::Scene>();
    VE::SceneSerializer dstSerializer(dstScene);
    CHECK(dstSerializer.DeserializeFromString(yaml));

    auto view = dstScene->GetAllEntitiesWith<VE::DirectionalLightComponent>();
    int count = 0;
    for (auto ent : view) {
        auto& dl = dstScene->GetRegistry().get<VE::DirectionalLightComponent>(ent);
        CHECK(dl.Intensity == doctest::Approx(1.5f));
        CHECK(dl.Color[0] == doctest::Approx(1.0f));
        CHECK(dl.Color[1] == doctest::Approx(0.9f));
        CHECK(dl.Color[2] == doctest::Approx(0.8f));
        count++;
    }
    CHECK(count == 1);
}

TEST_CASE("Serialize empty scene produces valid YAML") {
    auto scene = std::make_shared<VE::Scene>();
    VE::SceneSerializer serializer(scene);
    std::string yaml = serializer.SerializeToString();
    CHECK_FALSE(yaml.empty());
}

TEST_CASE("Serialize preserves RigidbodyComponent") {
    auto srcScene = std::make_shared<VE::Scene>();
    auto entity = srcScene->CreateEntity("PhysicsBox");
    auto& rb = entity.AddComponent<VE::RigidbodyComponent>();
    rb.Type = VE::BodyType::Dynamic;
    rb.Mass = 5.0f;
    rb.Restitution = 0.7f;
    rb.Friction = 0.3f;

    VE::SceneSerializer srcSerializer(srcScene);
    std::string yaml = srcSerializer.SerializeToString();

    auto dstScene = std::make_shared<VE::Scene>();
    VE::SceneSerializer dstSerializer(dstScene);
    CHECK(dstSerializer.DeserializeFromString(yaml));

    auto view = dstScene->GetAllEntitiesWith<VE::RigidbodyComponent>();
    int count = 0;
    for (auto ent : view) {
        auto& r = dstScene->GetRegistry().get<VE::RigidbodyComponent>(ent);
        CHECK(r.Type == VE::BodyType::Dynamic);
        CHECK(r.Mass == doctest::Approx(5.0f));
        CHECK(r.Restitution == doctest::Approx(0.7f));
        CHECK(r.Friction == doctest::Approx(0.3f));
        count++;
    }
    CHECK(count == 1);
}

} // TEST_SUITE("SceneSerialization")


// ═══════════════════════════════════════════════════════════════════════
// 5. Math / Frustum Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("Math") {

TEST_CASE("Plane distance-to-point") {
    VE::Plane plane;
    plane.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
    plane.Distance = 0.0f;

    CHECK(plane.DistanceTo(glm::vec3(0, 5, 0)) == doctest::Approx(5.0f));
    CHECK(plane.DistanceTo(glm::vec3(0, -3, 0)) == doctest::Approx(-3.0f));
    CHECK(plane.DistanceTo(glm::vec3(10, 0, 10)) == doctest::Approx(0.0f));
}

TEST_CASE("Plane normalization") {
    VE::Plane plane;
    plane.Normal = glm::vec3(0.0f, 2.0f, 0.0f);
    plane.Distance = 4.0f;
    plane.Normalize();

    CHECK(glm::length(plane.Normal) == doctest::Approx(1.0f));
    CHECK(plane.Distance == doctest::Approx(2.0f));
}

TEST_CASE("Frustum culling - AABB inside") {
    // Create a simple perspective projection looking down -Z
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    VE::Frustum frustum(proj * view);

    // AABB at the origin should be inside the frustum
    CHECK(frustum.TestAABB(glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1)));
}

TEST_CASE("Frustum culling - AABB outside") {
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    VE::Frustum frustum(proj * view);

    // AABB far behind the camera should be outside
    CHECK_FALSE(frustum.TestAABB(glm::vec3(-1, -1, 200), glm::vec3(1, 1, 202)));
}

TEST_CASE("Frustum sphere test") {
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    VE::Frustum frustum(proj * view);

    // Sphere at origin with radius 1 should be visible
    CHECK(frustum.TestSphere(glm::vec3(0, 0, 0), 1.0f));

    // Sphere far behind camera should not be visible
    CHECK_FALSE(frustum.TestSphere(glm::vec3(0, 0, 300), 1.0f));
}

TEST_CASE("AABB center and extents") {
    VE::AABB aabb;
    aabb.Min = glm::vec3(-2, -3, -4);
    aabb.Max = glm::vec3(2, 3, 4);

    auto center = aabb.Center();
    CHECK(center.x == doctest::Approx(0.0f));
    CHECK(center.y == doctest::Approx(0.0f));
    CHECK(center.z == doctest::Approx(0.0f));

    auto extents = aabb.Extents();
    CHECK(extents.x == doctest::Approx(2.0f));
    CHECK(extents.y == doctest::Approx(3.0f));
    CHECK(extents.z == doctest::Approx(4.0f));
}

TEST_CASE("AABB validity") {
    VE::AABB valid;
    valid.Min = glm::vec3(-1);
    valid.Max = glm::vec3(1);
    CHECK(valid.Valid());

    VE::AABB invalid; // default has Min > Max
    CHECK_FALSE(invalid.Valid());
}

} // TEST_SUITE("Math")


// ═══════════════════════════════════════════════════════════════════════
// 6. Material System Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("Material") {

TEST_CASE("Create material with null shader") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    CHECK(mat != nullptr);
    CHECK(mat->GetName() == "TestMat");
    CHECK(mat->GetShader() == nullptr);
}

TEST_CASE("Set and get float property") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    mat->SetFloat("_Roughness", 0.75f);

    auto& props = mat->GetProperties();
    REQUIRE(props.size() == 1);
    CHECK(props[0].Name == "_Roughness");
    CHECK(props[0].Type == VE::MaterialPropertyType::Float);
    CHECK(props[0].FloatValue == doctest::Approx(0.75f));
}

TEST_CASE("Set and get int property") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    mat->SetInt("_TileCount", 4);

    auto& props = mat->GetProperties();
    REQUIRE(props.size() == 1);
    CHECK(props[0].IntValue == 4);
}

TEST_CASE("Set and get vec3 property") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    mat->SetVec3("_Tint", glm::vec3(0.1f, 0.2f, 0.3f));

    auto& props = mat->GetProperties();
    REQUIRE(props.size() == 1);
    CHECK(props[0].Vec3Value.x == doctest::Approx(0.1f));
    CHECK(props[0].Vec3Value.y == doctest::Approx(0.2f));
    CHECK(props[0].Vec3Value.z == doctest::Approx(0.3f));
}

TEST_CASE("Set and get vec4 property") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    mat->SetVec4("_Color", glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));

    auto& props = mat->GetProperties();
    REQUIRE(props.size() == 1);
    CHECK(props[0].Vec4Value.r == doctest::Approx(1.0f));
    CHECK(props[0].Vec4Value.g == doctest::Approx(0.5f));
    CHECK(props[0].Vec4Value.b == doctest::Approx(0.0f));
    CHECK(props[0].Vec4Value.a == doctest::Approx(1.0f));
}

TEST_CASE("Overwrite existing property updates value") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    mat->SetFloat("_Metallic", 0.5f);
    mat->SetFloat("_Metallic", 0.9f);

    auto& props = mat->GetProperties();
    CHECK(props.size() == 1); // should not duplicate
    CHECK(props[0].FloatValue == doctest::Approx(0.9f));
}

TEST_CASE("Multiple properties coexist") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    mat->SetFloat("_Roughness", 0.5f);
    mat->SetInt("_Mode", 2);
    mat->SetVec4("_Color", glm::vec4(1.0f));

    CHECK(mat->GetProperties().size() == 3);
}

TEST_CASE("Material lit flag") {
    auto mat = VE::Material::Create("TestMat", nullptr);
    CHECK_FALSE(mat->IsLit());
    mat->SetLit(true);
    CHECK(mat->IsLit());
}

} // TEST_SUITE("Material")


// ═══════════════════════════════════════════════════════════════════════
// 7. ShaderLab Parser Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("ShaderLab") {

static const char* kTestShader = R"(
Shader "Test/SimpleUnlit" {
    Properties {
        _Color ("Main Color", Color) = (1, 0.5, 0, 1)
        _Brightness ("Brightness", Float) = 1.0
        _MainTex ("Texture", 2D) = "white" {}
    }
    SubShader {
        Tags { "RenderType"="Opaque" }
        Pass {
            Name "ForwardBase"
            Cull Back
            ZWrite On

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #ifdef VERTEX
            void main() { gl_Position = vec4(0); }
            #endif

            #ifdef FRAGMENT
            void main() { fragColor = vec4(1); }
            #endif
            ENDGLSL
        }
    }
    FallBack "Diffuse"
}
)";

TEST_CASE("Parse shader name") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    bool ok = parser.Parse(shader);
    CHECK(ok);
    CHECK(shader.Name == "Test/SimpleUnlit");
}

TEST_CASE("Parse properties") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    REQUIRE(parser.Parse(shader));

    CHECK(shader.Properties.size() == 3);

    // Color property
    CHECK(shader.Properties[0].Name == "_Color");
    CHECK(shader.Properties[0].DisplayName == "Main Color");
    CHECK(shader.Properties[0].Type == VE::ShaderLabPropertyType::Color);

    // Float property
    CHECK(shader.Properties[1].Name == "_Brightness");
    CHECK(shader.Properties[1].Type == VE::ShaderLabPropertyType::Float);
    CHECK(shader.Properties[1].FloatDefault == doctest::Approx(1.0f));

    // Texture property
    CHECK(shader.Properties[2].Name == "_MainTex");
    CHECK(shader.Properties[2].Type == VE::ShaderLabPropertyType::Texture2D);
    CHECK(shader.Properties[2].TextureDefault == "white");
}

TEST_CASE("Parse SubShader and Pass") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    REQUIRE(parser.Parse(shader));

    REQUIRE(shader.SubShaders.size() == 1);
    auto& subShader = shader.SubShaders[0];

    // Tags
    CHECK(subShader.Tags.count("RenderType") == 1);
    CHECK(subShader.Tags.at("RenderType") == "Opaque");

    // Pass
    REQUIRE(subShader.Passes.size() == 1);
    auto& pass = subShader.Passes[0];
    CHECK(pass.Name == "ForwardBase");
}

TEST_CASE("Parse render state") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    REQUIRE(parser.Parse(shader));

    auto& pass = shader.SubShaders[0].Passes[0];
    CHECK(pass.RenderState.Cull == VE::CullMode::Back);
    CHECK(pass.RenderState.ZWrite == VE::ZWriteMode::On);
}

TEST_CASE("Parse GLSL block extracts source") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    REQUIRE(parser.Parse(shader));

    auto& pass = shader.SubShaders[0].Passes[0];
    CHECK_FALSE(pass.RawGLSL.empty());
}

TEST_CASE("Parse FallBack") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    REQUIRE(parser.Parse(shader));
    CHECK(shader.FallBack == "Diffuse");
}

TEST_CASE("Extract GLSL vertex and fragment source") {
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(kTestShader);
    REQUIRE(parser.Parse(shader));

    auto& pass = shader.SubShaders[0].Passes[0];
    std::string vertSrc, fragSrc;
    bool ok = VE::ShaderLabCompiler::ExtractGLSL(pass.RawGLSL, pass.VertexEntry,
                                                  pass.FragmentEntry, vertSrc, fragSrc);
    CHECK(ok);
    CHECK_FALSE(vertSrc.empty());
    CHECK_FALSE(fragSrc.empty());
}

TEST_CASE("Invalid shader produces parse errors") {
    const char* bad = R"(Shader { broken )";
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(bad);
    bool ok = parser.Parse(shader);
    CHECK_FALSE(ok);
    CHECK_FALSE(parser.GetErrors().empty());
}

TEST_CASE("Parse Range property") {
    const char* src = R"(
Shader "Test/Range" {
    Properties {
        _Smoothness ("Smoothness", Range(0, 1)) = 0.5
    }
    SubShader {
        Pass {
            GLSLPROGRAM
            #ifdef VERTEX
            void main() {}
            #endif
            #ifdef FRAGMENT
            void main() {}
            #endif
            ENDGLSL
        }
    }
}
)";
    VE::ShaderLabShader shader;
    VE::ShaderLabParser parser(src);
    REQUIRE(parser.Parse(shader));
    REQUIRE(shader.Properties.size() == 1);
    CHECK(shader.Properties[0].Type == VE::ShaderLabPropertyType::Range);
    CHECK(shader.Properties[0].RangeMin == doctest::Approx(0.0f));
    CHECK(shader.Properties[0].RangeMax == doctest::Approx(1.0f));
    CHECK(shader.Properties[0].FloatDefault == doctest::Approx(0.5f));
}

} // TEST_SUITE("ShaderLab")


// ═══════════════════════════════════════════════════════════════════════
// 8. Input Action Mapping Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("InputAction") {

TEST_CASE("Create action with name and type") {
    VE::InputAction action("Jump", VE::ActionType::Button);
    CHECK(action.GetName() == "Jump");
    CHECK(action.GetType() == VE::ActionType::Button);
}

TEST_CASE("Add bindings to action") {
    VE::InputAction action("Jump", VE::ActionType::Button);
    VE::InputBinding binding;
    binding.Source = VE::BindingSource::Key;
    binding.Code = 32; // space key
    action.AddBinding(binding);

    CHECK(action.GetBindings().size() == 1);
    CHECK(action.GetBindings()[0].Code == 32);
}

TEST_CASE("Clear bindings") {
    VE::InputAction action("Jump", VE::ActionType::Button);
    VE::InputBinding b1, b2;
    b1.Code = 32;
    b2.Code = 87;
    action.AddBinding(b1);
    action.AddBinding(b2);
    CHECK(action.GetBindings().size() == 2);

    action.ClearBindings();
    CHECK(action.GetBindings().empty());
}

TEST_CASE("InputActionMap add and get action") {
    VE::InputActionMap map("Player");
    CHECK(map.GetName() == "Player");

    auto& jump = map.AddAction("Jump", VE::ActionType::Button);
    CHECK(jump.GetName() == "Jump");

    auto& move = map.AddAction("MoveHorizontal", VE::ActionType::Axis);
    CHECK(move.GetType() == VE::ActionType::Axis);

    VE::InputAction* found = map.GetAction("Jump");
    REQUIRE(found != nullptr);
    CHECK(found->GetName() == "Jump");

    CHECK(map.GetAction("NonExistent") == nullptr);
}

TEST_CASE("InputActionMap enable/disable") {
    VE::InputActionMap map("UI");
    CHECK(map.IsEnabled());

    map.SetEnabled(false);
    CHECK_FALSE(map.IsEnabled());
}

TEST_CASE("Multiple bindings on one action") {
    VE::InputAction action("Fire", VE::ActionType::Button);

    VE::InputBinding keyBinding;
    keyBinding.Source = VE::BindingSource::Key;
    keyBinding.Code = 70; // F key

    VE::InputBinding mouseBinding;
    mouseBinding.Source = VE::BindingSource::MouseButton;
    mouseBinding.Code = 0; // left click

    action.AddBinding(keyBinding);
    action.AddBinding(mouseBinding);

    CHECK(action.GetBindings().size() == 2);
    CHECK(action.GetBindings()[0].Source == VE::BindingSource::Key);
    CHECK(action.GetBindings()[1].Source == VE::BindingSource::MouseButton);
}

} // TEST_SUITE("InputAction")


// ═══════════════════════════════════════════════════════════════════════
// 9. Component Default Value Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("Components") {

TEST_CASE("TransformComponent defaults") {
    VE::TransformComponent tc;
    CHECK(tc.Position[0] == doctest::Approx(0.0f));
    CHECK(tc.Position[1] == doctest::Approx(0.0f));
    CHECK(tc.Position[2] == doctest::Approx(0.0f));
    CHECK(tc.Rotation[0] == doctest::Approx(0.0f));
    CHECK(tc.Scale[0] == doctest::Approx(1.0f));
    CHECK(tc.Scale[1] == doctest::Approx(1.0f));
    CHECK(tc.Scale[2] == doctest::Approx(1.0f));
}

TEST_CASE("DirectionalLightComponent defaults") {
    VE::DirectionalLightComponent dl;
    CHECK(dl.Intensity == doctest::Approx(1.0f));
    CHECK(dl.Color[0] == doctest::Approx(1.0f));
    CHECK(dl.Color[1] == doctest::Approx(1.0f));
    CHECK(dl.Color[2] == doctest::Approx(1.0f));
}

TEST_CASE("RigidbodyComponent defaults") {
    VE::RigidbodyComponent rb;
    CHECK(rb.Type == VE::BodyType::Dynamic);
    CHECK(rb.Mass == doctest::Approx(1.0f));
    CHECK(rb.UseGravity == true);
}

TEST_CASE("TagComponent defaults") {
    VE::TagComponent tag;
    CHECK(tag.GameObjectTag == "Untagged");
    CHECK(tag.Layer == 0);
    CHECK(tag.Active == true);
}

TEST_CASE("CameraComponent defaults") {
    VE::CameraComponent cam;
    CHECK(cam.ProjectionType == VE::CameraProjection::Perspective);
    CHECK(cam.FOV == doctest::Approx(60.0f));
    CHECK(cam.NearClip == doctest::Approx(0.1f));
    CHECK(cam.FarClip == doctest::Approx(1000.0f));
}

TEST_CASE("BoxColliderComponent defaults") {
    VE::BoxColliderComponent bc;
    CHECK(bc.Size[0] == doctest::Approx(1.0f));
    CHECK(bc.Size[1] == doctest::Approx(1.0f));
    CHECK(bc.Size[2] == doctest::Approx(1.0f));
    CHECK(bc.Offset[0] == doctest::Approx(0.0f));
}

TEST_CASE("SphereColliderComponent defaults") {
    VE::SphereColliderComponent sc;
    CHECK(sc.Radius == doctest::Approx(0.5f));
}

TEST_CASE("AudioSourceComponent defaults") {
    VE::AudioSourceComponent as;
    CHECK(as.Volume == doctest::Approx(1.0f));
    CHECK(as.Pitch == doctest::Approx(1.0f));
    CHECK(as.Loop == false);
    CHECK(as.PlayOnAwake == true);
    CHECK(as.Spatial == false);
}

} // TEST_SUITE("Components")


// ═══════════════════════════════════════════════════════════════════════
// 10. ShaderLab Lexer Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("ShaderLabLexer") {

TEST_CASE("Lexer tokenizes keywords") {
    VE::ShaderLabLexer lexer("Shader Properties SubShader Pass");

    auto t1 = lexer.NextToken();
    CHECK(t1.Type == VE::ShaderLabTokenType::KW_Shader);

    auto t2 = lexer.NextToken();
    CHECK(t2.Type == VE::ShaderLabTokenType::KW_Properties);

    auto t3 = lexer.NextToken();
    CHECK(t3.Type == VE::ShaderLabTokenType::KW_SubShader);

    auto t4 = lexer.NextToken();
    CHECK(t4.Type == VE::ShaderLabTokenType::KW_Pass);
}

TEST_CASE("Lexer tokenizes strings") {
    VE::ShaderLabLexer lexer("\"Hello World\"");
    auto tok = lexer.NextToken();
    CHECK(tok.Type == VE::ShaderLabTokenType::String);
    CHECK(tok.Value == "Hello World");
}

TEST_CASE("Lexer tokenizes numbers") {
    VE::ShaderLabLexer lexer("42 3.14 -1.5");

    auto t1 = lexer.NextToken();
    CHECK(t1.Type == VE::ShaderLabTokenType::Number);
    CHECK(t1.Value == "42");

    auto t2 = lexer.NextToken();
    CHECK(t2.Type == VE::ShaderLabTokenType::Number);
    CHECK(t2.Value == "3.14");

    // -1.5 may be tokenized as a negative number
    auto t3 = lexer.NextToken();
    CHECK(t3.Type == VE::ShaderLabTokenType::Number);
}

TEST_CASE("Lexer tokenizes symbols") {
    VE::ShaderLabLexer lexer("{ } ( ) = ,");

    CHECK(lexer.NextToken().Type == VE::ShaderLabTokenType::LeftBrace);
    CHECK(lexer.NextToken().Type == VE::ShaderLabTokenType::RightBrace);
    CHECK(lexer.NextToken().Type == VE::ShaderLabTokenType::LeftParen);
    CHECK(lexer.NextToken().Type == VE::ShaderLabTokenType::RightParen);
    CHECK(lexer.NextToken().Type == VE::ShaderLabTokenType::Equals);
    CHECK(lexer.NextToken().Type == VE::ShaderLabTokenType::Comma);
}

TEST_CASE("Lexer reaches EOF") {
    VE::ShaderLabLexer lexer("");
    auto tok = lexer.NextToken();
    CHECK(tok.Type == VE::ShaderLabTokenType::EndOfFile);
}

TEST_CASE("Lexer PeekToken does not consume") {
    VE::ShaderLabLexer lexer("Shader");
    auto peeked = lexer.PeekToken();
    CHECK(peeked.Type == VE::ShaderLabTokenType::KW_Shader);
    auto consumed = lexer.NextToken();
    CHECK(consumed.Type == VE::ShaderLabTokenType::KW_Shader);
}

} // TEST_SUITE("ShaderLabLexer")


// ═══════════════════════════════════════════════════════════════════════
// 11. NavGrid Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("NavGrid") {

TEST_CASE("NavGrid walkability") {
    VE::NavGrid grid;
    grid.Width = 4;
    grid.Height = 4;
    grid.CellSize = 1.0f;
    grid.OriginX = 0.0f;
    grid.OriginZ = 0.0f;
    grid.Cells.resize(16, 0); // all walkable

    CHECK(grid.IsWalkable(0, 0));
    CHECK(grid.IsWalkable(3, 3));

    // Block a cell
    grid.Cells[1 * 4 + 2] = 1; // row 1, col 2
    CHECK_FALSE(grid.IsWalkable(2, 1));

    // Out of bounds
    CHECK_FALSE(grid.IsWalkable(-1, 0));
    CHECK_FALSE(grid.IsWalkable(4, 0));
    CHECK_FALSE(grid.IsWalkable(0, 4));
}

TEST_CASE("NavGrid world-to-grid conversion") {
    VE::NavGrid grid;
    grid.Width = 10;
    grid.Height = 10;
    grid.CellSize = 0.5f;
    grid.OriginX = -2.5f;
    grid.OriginZ = -2.5f;
    grid.Cells.resize(100, 0);

    int gx, gz;
    grid.WorldToGrid(0.0f, 0.0f, gx, gz);
    CHECK(gx == 5);
    CHECK(gz == 5);

    grid.WorldToGrid(-2.5f, -2.5f, gx, gz);
    CHECK(gx == 0);
    CHECK(gz == 0);
}

} // TEST_SUITE("NavGrid")


// ═══════════════════════════════════════════════════════════════════════
// 12. RenderPipelineSettings Defaults
// ═══════════════════════════════════════════════════════════════════════

TEST_SUITE("RenderPipelineSettings") {

TEST_CASE("Default settings are sensible") {
    VE::RenderPipelineSettings settings;
    CHECK(settings.HDREnabled == true);
    CHECK(settings.Exposure == doctest::Approx(1.0f));
    CHECK(settings.SkyEnabled == true);
    CHECK(settings.BloomEnabled == false);
    CHECK(settings.SSAOEnabled == false);
    CHECK(settings.AAMode == 0); // none
    CHECK(settings.FogEnabled == false);
}

TEST_CASE("Pipeline settings can be modified via Scene") {
    VE::Scene scene;
    auto& settings = scene.GetPipelineSettings();
    settings.BloomEnabled = true;
    settings.BloomThreshold = 0.5f;

    CHECK(scene.GetPipelineSettings().BloomEnabled == true);
    CHECK(scene.GetPipelineSettings().BloomThreshold == doctest::Approx(0.5f));
}

} // TEST_SUITE("RenderPipelineSettings")
