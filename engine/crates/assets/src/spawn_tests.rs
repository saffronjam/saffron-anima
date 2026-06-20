//! Spawn / instantiate tests (GPU-free: spawning is pure scene-ECS mutation over the
//! container META; no upload).

use crate::error::Error;
use crate::{AssetServer, ImportOptions};
use saffron_core::Uuid;
use saffron_geometry::glam::{Mat4, Quat, Vec3};
use saffron_geometry::{
    ImportedMaterial, ImportedModel, ImportedNode, ImportedSkin, Mesh, SkinPayload, Submesh,
    Vertex, VertexSkin,
};
use saffron_scene::{
    AnimationPlayer, Bone, BonePhysicsComponent, Material, MaterialSet, ModelInstance, Scene,
    SkinnedMesh,
};
use std::path::PathBuf;

/// A unique scratch dir under the system temp, removed and recreated per test.
fn scratch(tag: &str) -> PathBuf {
    let dir =
        std::env::temp_dir().join(format!("saffron-assets-spawn-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// A one-submesh triangle mesh (the minimal renderable shape).
fn tri_mesh() -> Mesh {
    Mesh {
        vertices: vec![
            Vertex {
                position: Vec3::ZERO,
                normal: Vec3::Z,
                uv0: saffron_geometry::glam::Vec2::ZERO,
            },
            Vertex {
                position: Vec3::X,
                normal: Vec3::Z,
                uv0: saffron_geometry::glam::Vec2::new(1.0, 0.0),
            },
            Vertex {
                position: Vec3::Y,
                normal: Vec3::Z,
                uv0: saffron_geometry::glam::Vec2::new(0.0, 1.0),
            },
        ],
        indices: vec![0, 1, 2],
        submeshes: vec![Submesh {
            first_index: 0,
            index_count: 3,
            vertex_offset: 0,
            material_slot: 0,
        }],
    }
}

/// A two-submesh quad (slots 0 and 1) so a multi-material spawn produces a `MaterialSet`.
fn quad_mesh() -> Mesh {
    let mut mesh = tri_mesh();
    mesh.vertices.push(Vertex {
        position: Vec3::ONE,
        normal: Vec3::Z,
        uv0: saffron_geometry::glam::Vec2::ONE,
    });
    mesh.indices = vec![0, 1, 2, 0, 2, 3];
    mesh.submeshes = vec![
        Submesh {
            first_index: 0,
            index_count: 3,
            vertex_offset: 0,
            material_slot: 0,
        },
        Submesh {
            first_index: 3,
            index_count: 3,
            vertex_offset: 0,
            material_slot: 1,
        },
    ];
    mesh
}

/// Bakes `graph` into the server's catalog, returning the model id ready to instantiate.
fn bake_into_catalog(assets: &mut AssetServer, graph: &ImportedModel, source: &str) -> Uuid {
    let bake = assets
        .bake_model(graph, ImportOptions::default(), source, Uuid(0))
        .expect("bake");
    for row in &bake.rows {
        assets.catalog.put(row.clone());
    }
    bake.model_id
}

#[test]
fn instantiate_flat_model_spawns_one_mesh_entity_with_base_color() {
    let dir = scratch("flat");
    let root = dir.join("assets");
    let mut assets = AssetServer::new(&root);
    let graph = ImportedModel {
        mesh: tri_mesh(),
        materials: vec![ImportedMaterial {
            name: "flat".to_owned(),
            base_color: saffron_geometry::glam::Vec4::new(0.25, 0.5, 0.75, 1.0),
            metallic: 0.3,
            roughness: 0.4,
            ..ImportedMaterial::default()
        }],
        skin: None,
    };
    let model_id = bake_into_catalog(&mut assets, &graph, "/tmp/flat.obj");

    let mut scene = Scene::new();
    let entity = assets
        .instantiate_model(&mut scene, model_id, "Cube")
        .expect("instantiate");

    // One material -> an inline Material with the baked sub-id's base color.
    assert!(scene.has_component::<saffron_scene::Mesh>(entity));
    assert!(scene.has_component::<Material>(entity));
    assert!(!scene.has_component::<MaterialSet>(entity));
    assert!(scene.has_component::<ModelInstance>(entity));

    let mesh_id = scene.component::<saffron_scene::Mesh>(entity).unwrap().mesh;
    let baked_mesh = assets
        .load_model_asset(model_id)
        .unwrap()
        .meta
        .sub_assets
        .iter()
        .find(|s| s.asset_type == saffron_scene::AssetType::Mesh)
        .unwrap()
        .sub_id;
    assert_eq!(
        mesh_id, baked_mesh,
        "the spawned mesh id is the baked sub-id"
    );

    let material = scene.component::<Material>(entity).unwrap();
    assert!((material.base_color.x - 0.25).abs() < 1e-5);
    assert!((material.base_color.z - 0.75).abs() < 1e-5);

    let instance = scene.component::<ModelInstance>(entity).unwrap();
    assert_eq!(instance.model_id, model_id);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn instantiate_multi_material_model_spawns_a_material_set_in_slot_order() {
    let dir = scratch("multimat");
    let root = dir.join("assets");
    let mut assets = AssetServer::new(&root);
    let graph = ImportedModel {
        mesh: quad_mesh(),
        materials: vec![
            ImportedMaterial {
                name: "a".to_owned(),
                base_color: saffron_geometry::glam::Vec4::new(1.0, 0.0, 0.0, 1.0),
                ..ImportedMaterial::default()
            },
            ImportedMaterial {
                name: "b".to_owned(),
                base_color: saffron_geometry::glam::Vec4::new(0.0, 1.0, 0.0, 1.0),
                ..ImportedMaterial::default()
            },
        ],
        skin: None,
    };
    let model_id = bake_into_catalog(&mut assets, &graph, "/tmp/two.obj");

    let mut scene = Scene::new();
    let entity = assets
        .instantiate_model(&mut scene, model_id, "Two")
        .expect("instantiate");

    assert!(!scene.has_component::<Material>(entity));
    let set = scene
        .with_component::<MaterialSet, _>(entity, |s| s.slots.clone())
        .expect("a material set");
    assert_eq!(set.len(), 2, "two slots, in slot order");
    assert!(
        (set[0].base_color.x - 1.0).abs() < 1e-5,
        "slot 0 is material a"
    );
    assert!(
        (set[1].base_color.y - 1.0).abs() < 1e-5,
        "slot 1 is material b"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

/// A rigged graph: two nodes (root, joint) with a non-identity joint rotation, one joint,
/// one clip — so the skinned spawn path exercises the node forest, bone tagging, the skin
/// descriptor, the animation player, and the META quaternion decode.
fn rigged_graph() -> ImportedModel {
    let clip = saffron_geometry::AnimClip {
        name: "idle".to_owned(),
        duration: 1.0,
        tracks: vec![saffron_geometry::AnimTrack {
            joint: 1,
            joint_name: "joint".to_owned(),
            ..saffron_geometry::AnimTrack::default()
        }],
    };
    // A 90-degree rotation about Y, stored on the joint node.
    let joint_rotation = Quat::from_axis_angle(Vec3::Y, std::f32::consts::FRAC_PI_2);
    ImportedModel {
        mesh: tri_mesh(),
        materials: vec![ImportedMaterial {
            name: "skin".to_owned(),
            ..ImportedMaterial::default()
        }],
        skin: Some(SkinPayload {
            nodes: vec![
                ImportedNode {
                    name: "root".to_owned(),
                    ..ImportedNode::default()
                },
                ImportedNode {
                    name: "joint".to_owned(),
                    parent: 0,
                    translation: Vec3::new(0.0, 1.0, 0.0),
                    rotation: joint_rotation,
                    ..ImportedNode::default()
                },
            ],
            desc: ImportedSkin {
                joints: vec![1],
                inverse_bind: vec![Mat4::IDENTITY],
                skeleton_root: 0,
                mesh_node: 0,
            },
            animations: vec![clip],
            stream: vec![VertexSkin::default(); 3],
        }),
    }
}

#[test]
fn instantiate_skinned_model_spawns_node_forest_bones_and_skin() {
    let dir = scratch("skinned");
    let root = dir.join("assets");
    let mut assets = AssetServer::new(&root);
    let graph = rigged_graph();
    let model_id = bake_into_catalog(&mut assets, &graph, "/tmp/rig.glb");

    let mut scene = Scene::new();
    let container = assets
        .instantiate_model(&mut scene, model_id, "Rig")
        .expect("instantiate");

    // The container root carries ModelInstance.
    assert!(scene.has_component::<ModelInstance>(container));

    // Find the skinned-mesh entity by query.
    let mut skinned: Option<(Uuid, usize, Uuid)> = None;
    scene.for_each::<&SkinnedMesh, _>(|_, skin| {
        skinned = Some((skin.mesh, skin.bones.len(), skin.root_bone));
    });
    let (skin_mesh, bone_count, root_bone) = skinned.expect("a skinned mesh exists");
    assert_eq!(bone_count, 1, "one joint in the skin");
    assert_ne!(skin_mesh.value(), 0, "the skinned mesh has a real sub-id");
    assert_ne!(root_bone.value(), 0, "skeletonRoot resolves to a node uuid");

    // Exactly one bone tag (the single joint).
    let mut bones = 0;
    scene.for_each::<&Bone, _>(|_, _| bones += 1);
    assert_eq!(bones, 1, "the single joint is bone-tagged");

    // The animation player is attached with the first clip, stopped, looping.
    let mut player: Option<AnimationPlayer> = None;
    scene.for_each::<&AnimationPlayer, _>(|_, p| player = Some(*p));
    let player = player.expect("an animation player exists");
    assert!(!player.playing, "imported rigs spawn stopped");
    assert_ne!(player.clip.value(), 0, "the first clip id is attached");

    // Auto-fit ran: a BonePhysicsComponent with one bone entry.
    let mut phys_bones = None;
    scene.for_each::<&BonePhysicsComponent, _>(|_, p| phys_bones = Some(p.bones.len()));
    assert_eq!(phys_bones, Some(1), "auto-fit produced one bone capsule");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn meta_quaternion_decode_reorders_to_glam_xyzw() {
    // The import writes r=[w,x,y,z]; the decode must rebuild glam xyzw.
    let rot = Quat::from_axis_angle(Vec3::Y, std::f32::consts::FRAC_PI_2);
    let nodes = serde_json::json!([{
        "name": "j",
        "parent": -1,
        "t": [0.0, 0.0, 0.0],
        "r": [rot.w, rot.x, rot.y, rot.z],
        "s": [1.0, 1.0, 1.0],
    }]);
    let decoded = crate::spawn::imported_nodes_from_json(&nodes);
    assert_eq!(decoded.len(), 1);
    let q = decoded[0].rotation;
    assert!((q.x - rot.x).abs() < 1e-5);
    assert!((q.y - rot.y).abs() < 1e-5);
    assert!((q.z - rot.z).abs() < 1e-5);
    assert!((q.w - rot.w).abs() < 1e-5);
}

#[test]
fn instantiating_twice_yields_stable_soft_references() {
    let dir = scratch("twice");
    let root = dir.join("assets");
    let mut assets = AssetServer::new(&root);
    let graph = rigged_graph();
    let model_id = bake_into_catalog(&mut assets, &graph, "/tmp/rig.glb");

    let mut scene = Scene::new();
    let a = assets
        .instantiate_model(&mut scene, model_id, "Rig A")
        .expect("instantiate a");
    let b = assets
        .instantiate_model(&mut scene, model_id, "Rig B")
        .expect("instantiate b");
    assert_ne!(a, b, "two independent entity trees");

    // Both instances reference the same mesh sub-id (soft references stable).
    let mut meshes: Vec<Uuid> = Vec::new();
    scene.for_each::<&SkinnedMesh, _>(|_, skin| meshes.push(skin.mesh));
    assert_eq!(meshes.len(), 2);
    assert_eq!(
        meshes[0], meshes[1],
        "the same baked sub-id across instances"
    );

    let mut models: Vec<Uuid> = Vec::new();
    scene.for_each::<&ModelInstance, _>(|_, m| models.push(m.model_id));
    assert_eq!(models.len(), 2);
    assert!(models.iter().all(|m| *m == model_id));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_skinned_node_decode_recovers_the_joint_local_transform() {
    let skin = serde_json::json!({
        "joints": [1],
        "inverseBind": [[1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]],
        "skeletonRoot": 0,
        "meshNode": 0,
    });
    let decoded = crate::spawn::imported_skin_from_json(&skin);
    assert_eq!(decoded.joints, vec![1]);
    assert_eq!(decoded.skeleton_root, 0);
    assert_eq!(decoded.mesh_node, 0);
    assert_eq!(decoded.inverse_bind.len(), 1);
    assert_eq!(decoded.inverse_bind[0], Mat4::IDENTITY);

    // A null / non-object skin decodes to an empty descriptor.
    assert!(
        crate::spawn::imported_skin_from_json(&serde_json::Value::Null)
            .joints
            .is_empty()
    );
}

/// `instantiate_model` for an id that is not in the catalog returns `NotInCatalog`.
#[test]
fn instantiate_missing_model_errors() {
    let dir = scratch("missing");
    let mut assets = AssetServer::new(dir.join("assets"));
    let mut scene = Scene::new();
    let err = assets
        .instantiate_model(&mut scene, Uuid(424_242), "Nope")
        .unwrap_err();
    assert!(matches!(err, Error::NotInCatalog(424_242)));
    let _ = std::fs::remove_dir_all(&dir);
}
