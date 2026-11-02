extends SceneTree
## Small, self-contained regression for the fork's offline-baked resource API.

var failures: Array[String] = []
var checks := 0


func check(ok: bool, label: String) -> void:
	checks += 1
	if not ok:
		failures.append(label)
		push_error("BAKED_COLLISION_SMOKE_FAIL: " + label)


func _initialize() -> void:
	call_deferred("run")


func run() -> void:
	var vertices := PackedVector3Array([
		Vector3(-2, 0, -2), Vector3(-2, 0, 2),
		Vector3(2, 0, 2), Vector3(2, 0, -2),
	])
	var indices := PackedInt32Array([0, 1, 2, 0, 2, 3])
	var boxes := [{"center": Vector3(5, 1, 0), "size": Vector3(2, 2, 2)}]
	var baked = Box3DBakedCollision.bake(vertices, indices, boxes, 0.05)
	check(baked != null, "bake mesh and box")
	if baked == null:
		finish()
		return

	var info: Dictionary = baked.get_info()
	check(info.mesh_count == 1, "one mesh reported")
	check(info.hull_count == 1, "one box hull reported")
	check(info.triangle_count == 2, "two triangles reported")
	check(is_equal_approx(info.friction, 0.05), "friction retained")

	var bytes: PackedByteArray = baked.get_bytes()
	var loaded = Box3DBakedCollision.from_bytes(bytes)
	check(loaded != null, "load serialized compound")
	check(loaded != null and loaded.get_bytes() == bytes, "byte round trip")
	check(not baked.get_debug_geometry().is_empty(), "cooked debug geometry available")

	var worker := Thread.new()
	check(worker.start(func(): return Box3DBakedCollision.from_bytes(bytes)) == OK, "worker load starts")
	var worker_loaded = worker.wait_to_finish()
	check(worker_loaded != null and worker_loaded.get_info() == info, "worker owns loaded resource")

	var world := Box3DWorld.new()
	world.auto_step = false
	root.add_child(world)
	var body := Box3DBody.new()
	body.body_type = Box3DBody.STATIC
	body.invoke_contact_creation = false
	body.position = Vector3(10, 2, 0)
	body.baked_collision = worker_loaded
	world.add_child(body)
	check(body.get_compound_info().byte_count == bytes.size(), "body reports attached compound")

	baked = null
	loaded = null
	worker_loaded = null
	bytes.clear()
	await process_frame

	var mesh_hit: Dictionary = world.raycast(Vector3(10, 6, 0), Vector3(10, -5, 0))
	var box_hit: Dictionary = world.raycast(Vector3(15, 6, 0), Vector3(15, -5, 0))
	check(mesh_hit.get("hit", false) and mesh_hit.collider == body, "mesh ray query")
	check(box_hit.get("hit", false) and box_hit.collider == body, "box ray query")

	body.free()
	world.free()
	finish()


func finish() -> void:
	print("BAKED_COLLISION_SMOKE ", JSON.stringify({
		"checks": checks,
		"failures": failures,
		"passed": failures.is_empty(),
	}))
	quit(0 if failures.is_empty() else 1)
