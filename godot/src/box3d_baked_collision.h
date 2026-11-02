// Offline-baked compound collision resource. SPDX-License-Identifier: MIT
#pragma once
#include <box3d/box3d.h>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <vector>

namespace godot
{
// Immutable private aligned copy: callers never receive its backing allocation.
// The body's Ref keeps it alive until every attached native shape is destroyed.
class Box3DBakedCollision : public RefCounted
{
	GDCLASS( Box3DBakedCollision, RefCounted )
	struct alignas( 16 ) Block
	{
		uint8_t bytes[16];
	};
	std::vector<Block> storage;
	int byte_count = 0;
	bool assign_bytes( const uint8_t* bytes, int64_t count );

protected:
	static void _bind_methods();

public:
	static Ref<Box3DBakedCollision> bake( const PackedVector3Array& vertices,
										  const PackedInt32Array& indices,
										  const Array& boxes = Array(), double friction = 0.05,
										  const Array& hulls = Array() );
	static Ref<Box3DBakedCollision> bake_meshes( const Array& meshes, const Array& boxes = Array(),
												 double friction = 0.05,
												 const Array& hulls = Array() );
	static Ref<Box3DBakedCollision> from_bytes( const PackedByteArray& bytes );
	PackedByteArray get_bytes() const;
	Dictionary get_info() const;
	Array get_debug_geometry() const;
	const b3CompoundData* get_compound() const
	{
		return byte_count ? reinterpret_cast<const b3CompoundData*>( storage.data() ) : nullptr;
	}
};
} // namespace godot
