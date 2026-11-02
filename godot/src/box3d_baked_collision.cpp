// Offline-baked compound collision resource. SPDX-License-Identifier: MIT
#include "box3d_baked_collision.h"

#include "box3d_conversions.h"

#include <box3d/constants.h>
#include <cmath>
#include <cstring>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <set>

using namespace godot;
namespace
{
constexpr int64_t MAX_BYTES = 512LL * 1024 * 1024;
constexpr int MAX_VERTICES = 1000000;
constexpr int MAX_TRIANGLES = 2000000;
constexpr int MAX_MESHES = 4096;
constexpr int MAX_CLOSED_HULLS = 65535;
constexpr int MAX_HULL_POINTS = 128;
// Factory temporaries are allocator-owned; the returned resource only owns a copy.
struct NativeTemps
{
	std::vector<b3HullData*> hulls;
	std::vector<b3MeshData*> meshes;
	b3CompoundData* compound = nullptr;
	~NativeTemps()
	{
		if ( compound )
			b3DestroyCompound( compound );
		for ( auto* mesh : meshes )
			b3DestroyMesh( mesh );
		for ( auto* hull : hulls )
			b3DestroyHull( hull );
	}
};
bool noncoplanar( const std::vector<b3Vec3>& points )
{
	// Test the values after native float conversion, which can collapse input.
	auto v = []( b3Vec3 p ) { return Vector3( p.x, p.y, p.z ); };
	const Vector3 origin = v( points[0] );
	Vector3 axis, normal;
	real_t axis_length = 0, normal_length = 0;
	for ( const auto& p : points )
	{
		const Vector3 delta = v( p ) - origin;
		const real_t length = delta.length_squared();
		if ( length > axis_length )
		{
			axis = delta;
			axis_length = length;
		}
	}
	if ( axis_length == 0 )
		return false;
	for ( const auto& p : points )
	{
		const Vector3 cross = axis.cross( v( p ) - origin );
		const real_t length = cross.length_squared();
		if ( length > normal_length )
		{
			normal = cross;
			normal_length = length;
		}
	}
	if ( normal_length == 0 )
		return false;
	for ( const auto& p : points )
	{
		if ( normal.dot( v( p ) - origin ) != 0 )
			return true;
	}
	return false;
}
// Exact private serialized instance layouts in pinned src/compound.c.
struct HullInstance
{
	b3Transform transform;
	uint32_t offset;
	uint16_t material, padding;
};
struct MeshInstance
{
	b3Transform transform;
	b3Vec3 scale;
	uint32_t offset, material_offset, material_count;
};
bool range( int64_t offset, int64_t count, int64_t stride, int64_t bytes, int alignment = 8 )
{
	return offset >= int64_t( sizeof( b3CompoundData ) ) && !( offset & ( alignment - 1 ) ) &&
		   count >= 0 && count <= MAX_TRIANGLES * 2LL && offset <= bytes &&
		   count * stride <= bytes - offset;
}
bool finite( const Vector3& v )
{
	// Keep double-precision Godot input safely inside native float arithmetic.
	return v.is_finite() && std::abs( v.x ) <= 1e7 && std::abs( v.y ) <= 1e7 &&
		   std::abs( v.z ) <= 1e7;
}
bool validate( const b3CompoundData* c, int count )
{
	if ( !b3ValidateCompound( reinterpret_cast<const uint8_t*>( c ), count ) )
		return false;
	// This intentionally accepts only the narrow format emitted by bake.
	if ( c->capsuleCount || c->sphereCount || c->meshCount < 0 || c->meshCount > MAX_MESHES ||
		 c->hullCount < 0 || c->hullCount > MAX_CLOSED_HULLS || c->meshCount + c->hullCount == 0 ||
		 c->meshCount + c->hullCount >= B3_MAX_CHILD_SHAPES || c->materialCount != 1 ||
		 c->sharedMeshCount < ( c->meshCount ? 1 : 0 ) || c->sharedMeshCount > c->meshCount ||
		 c->sharedHullCount < 0 || c->sharedHullCount > c->hullCount || c->treeHeight < 0 ||
		 c->treeHeight > 128 || c->nodeCount > 2 * ( c->hullCount + c->meshCount ) + 2 ||
		 !range( c->materialOffset, 1, sizeof( b3SurfaceMaterial ), count ) ||
		 !range( c->hullOffset, c->hullCount, sizeof( HullInstance ), count ) ||
		 !range( c->meshOffset, c->meshCount, sizeof( MeshInstance ), count ) )
		return false;
	const auto* base = reinterpret_cast<const uint8_t*>( c );
	const auto* hulls = reinterpret_cast<const HullInstance*>( base + c->hullOffset );
	for ( int i = 0; i < c->hullCount; ++i )
	{
		if ( hulls[i].material != 0 || !range( hulls[i].offset, 1, sizeof( b3HullData ), count ) )
			return false;
		const auto* h = reinterpret_cast<const b3HullData*>( base + hulls[i].offset );
		if ( h->version != B3_HULL_VERSION || h->byteCount < int( sizeof( b3HullData ) ) ||
			 int64_t( hulls[i].offset ) + h->byteCount > count || h->vertexCount < 4 ||
			 h->vertexCount > B3_MAX_HULL_VERTICES || h->faceCount < 4 ||
			 h->faceCount > B3_MAX_HULL_FACES || h->edgeCount < 12 ||
			 h->edgeCount > 2 * B3_MAX_HULL_EDGES || ( h->edgeCount & 1 ) )
			return false;
	}
	std::set<uint32_t> mesh_offsets;
	const auto* meshes = reinterpret_cast<const MeshInstance*>( base + c->meshOffset );
	for ( int i = 0; i < c->meshCount; ++i )
	{
		const auto* m = meshes + i;
		if ( m->material_count != 1 || !range( m->offset, 1, sizeof( b3MeshData ), count ) ||
			 !range( m->material_offset, 1, sizeof( uint16_t ), count, alignof( uint16_t ) ) )
			return false;
		const auto* mesh = reinterpret_cast<const b3MeshData*>( base + m->offset );
		if ( mesh->version != B3_MESH_VERSION || mesh->byteCount < int( sizeof( b3MeshData ) ) ||
			 int64_t( m->offset ) + mesh->byteCount > count || mesh->vertexCount < 3 ||
			 mesh->vertexCount > MAX_VERTICES || mesh->triangleCount < 1 ||
			 mesh->triangleCount > MAX_TRIANGLES || mesh->materialCount != 1 ||
			 *reinterpret_cast<const uint16_t*>( base + m->material_offset ) != 0 )
			return false;
		mesh_offsets.insert( m->offset );
	}
	return int( mesh_offsets.size() ) == c->sharedMeshCount;
}
} // namespace

bool Box3DBakedCollision::assign_bytes( const uint8_t* bytes, int64_t count )
{
	if ( !bytes || count < int64_t( sizeof( b3CompoundData ) ) || count > MAX_BYTES )
		return false;
	// Copy first to obtain guaranteed alignment and private ownership, then validate.
	storage.resize( ( count + 15 ) / 16 );
	std::memcpy( storage.data(), bytes, size_t( count ) );
	if ( !validate( reinterpret_cast<const b3CompoundData*>( storage.data() ), int( count ) ) )
	{
		storage.clear();
		return false;
	}
	byte_count = int( count );
	return true;
}

Ref<Box3DBakedCollision> Box3DBakedCollision::bake( const PackedVector3Array& vertices,
													const PackedInt32Array& indices,
													const Array& boxes, double friction,
													const Array& hulls )
{
	Array meshes;
	if ( vertices.size() || indices.size() )
	{
		Dictionary mesh;
		mesh["vertices"] = vertices;
		mesh["indices"] = indices;
		meshes.append( mesh );
	}
	return bake_meshes( meshes, boxes, friction, hulls );
}

Ref<Box3DBakedCollision> Box3DBakedCollision::bake_meshes( const Array& meshes, const Array& boxes,
														   double friction, const Array& hulls )
{
	Ref<Box3DBakedCollision> none;
	const int64_t mc = meshes.size(), bc = boxes.size(), hc = hulls.size();
	NativeTemps temporary;
	if ( !std::isfinite( friction ) || friction < 0 || friction > 1e6 || bc > MAX_CLOSED_HULLS ||
		 hc > MAX_CLOSED_HULLS - bc || mc > MAX_MESHES || mc + bc + hc >= B3_MAX_CHILD_SHAPES ||
		 ( !mc && !bc && !hc ) )
		return none;
	// Bound all input definitions before native allocation. This upper bound
	// includes a maximum-size mesh BVH (two nodes per input triangle), all
	// mesh instances/material maps, compound nodes, alignment and header.
	int64_t payload_bound =
		sizeof( b3CompoundData ) + sizeof( b3SurfaceMaterial ) + 64 +
		( mc + bc + hc ) * ( 2 * int64_t( sizeof( b3TreeNode ) ) + sizeof( MeshInstance ) + 16 );
	for ( int64_t i = 0; i < mc; ++i )
	{
		if ( meshes[i].get_type() != Variant::DICTIONARY )
			return none;
		Dictionary d = meshes[i];
		if ( !d.has( "vertices" ) || !d.has( "indices" ) ||
			 d["vertices"].get_type() != Variant::PACKED_VECTOR3_ARRAY ||
			 d["indices"].get_type() != Variant::PACKED_INT32_ARRAY )
			return none;
		const PackedVector3Array vertices = d["vertices"];
		const PackedInt32Array indices = d["indices"];
		const int64_t vc = vertices.size(), ic = indices.size();
		if ( vc < 3 || vc > MAX_VERTICES || ic < 3 || ic % 3 || ic / 3 > MAX_TRIANGLES )
			return none;
		payload_bound +=
			sizeof( b3MeshData ) + 40 + vc * sizeof( b3Vec3 ) +
			( ic / 3 ) * ( 2 * int64_t( sizeof( b3MeshNode ) ) + sizeof( b3MeshTriangle ) + 2 );
		if ( payload_bound > MAX_BYTES )
			return none;
	}
	b3SurfaceMaterial material = b3DefaultSurfaceMaterial();
	material.friction = float( friction );
	// Resize once: b3BoxHull has internal offsets and its address must stay stable.
	std::vector<b3BoxHull> box_hulls( size_t( bc ), b3BoxHull{} );
	std::vector<b3CompoundHullDef> hull_defs( size_t( bc + hc ), b3CompoundHullDef{} );
	for ( int64_t i = 0; i < bc; ++i )
	{
		if ( boxes[i].get_type() != Variant::DICTIONARY )
			return none;
		Dictionary d = boxes[i];
		if ( !d.has( "center" ) || !d.has( "size" ) || d["center"].get_type() != Variant::VECTOR3 ||
			 d["size"].get_type() != Variant::VECTOR3 )
			return none;
		Vector3 center = d["center"], size = d["size"];
		if ( !finite( center ) || !finite( size ) || size.x <= 0 || size.y <= 0 || size.z <= 0 )
			return none;
		box_hulls[size_t( i )] =
			b3MakeBoxHull( float( size.x * .5 ), float( size.y * .5 ), float( size.z * .5 ) );
		auto& h = hull_defs[size_t( i )];
		h.hull = &box_hulls[size_t( i )].base;
		h.transform = b3Transform_identity;
		h.transform.p = to_b3( center );
		h.material = material;
		payload_bound += sizeof( b3BoxHull ) + 8;
		if ( payload_bound > MAX_BYTES )
			return none;
	}
	for ( int64_t i = 0; i < hc; ++i )
	{
		if ( hulls[i].get_type() != Variant::PACKED_VECTOR3_ARRAY )
			return none;
		const PackedVector3Array input = hulls[i];
		if ( input.size() < 4 || input.size() > MAX_HULL_POINTS )
			return none;
		std::vector<b3Vec3> points( size_t( input.size() ), b3Vec3{} );
		for ( int64_t j = 0; j < input.size(); ++j )
		{
			if ( !finite( input[j] ) )
				return none;
			points[size_t( j )] = to_b3( input[j] );
		}
		if ( !noncoplanar( points ) )
			return none;
		b3HullData* hull =
			b3CreateHull( points.data(), int( points.size() ), int( points.size() ) );
		if ( !hull )
			return none;
		temporary.hulls.push_back( hull );
		if ( !std::isfinite( hull->volume ) || hull->volume <= 0 )
			return none;
		payload_bound += hull->byteCount + 8;
		if ( payload_bound > MAX_BYTES )
			return none;
		auto& h = hull_defs[size_t( bc + i )];
		h.hull = hull;
		h.transform = b3Transform_identity;
		h.material = material;
	}
	std::vector<b3CompoundMeshDef> mesh_defs( size_t( mc ), b3CompoundMeshDef{} );
	temporary.meshes.reserve( size_t( mc ) );
	for ( int64_t m = 0; m < mc; ++m )
	{
		Dictionary d = meshes[m];
		const PackedVector3Array vertices = d["vertices"];
		const PackedInt32Array indices = d["indices"];
		const int64_t vc = vertices.size(), ic = indices.size();
		std::vector<b3Vec3> verts( size_t( vc ), b3Vec3{} );
		std::vector<int32_t> idx( size_t( ic ), 0 );
		for ( int64_t i = 0; i < vc; ++i )
		{
			if ( !finite( vertices[i] ) )
				return none;
			verts[size_t( i )] = to_b3( vertices[i] );
		}
		for ( int64_t i = 0; i < ic; ++i )
		{
			if ( indices[i] < 0 || indices[i] >= vc )
				return none;
			idx[size_t( i )] = indices[i];
		}
		for ( int64_t i = 0; i < ic; i += 3 )
		{
			Vector3 a = vertices[idx[size_t( i )]], b = vertices[idx[size_t( i + 1 )]],
					c = vertices[idx[size_t( i + 2 )]];
			if ( ( b - a ).cross( c - a ).length_squared() <= 1e-16 )
				return none;
		}
		b3MeshDef md = {};
		md.vertices = verts.data();
		md.indices = idx.data();
		md.vertexCount = int( vc );
		md.triangleCount = int( ic / 3 );
		md.weldVertices = true;
		md.weldTolerance = .001f;
		md.useMedianSplit = true; // Preserve the established collision experiment cook policy.
		md.identifyEdges = true;
		// Raw native winding: same zero-initialized clockWiseWinding as Box3DBody.
		b3MeshData* mesh = b3CreateMesh( &md, nullptr, 0 );
		if ( !mesh )
			return none;
		temporary.meshes.push_back( mesh );
		auto& mesh_def = mesh_defs[size_t( m )];
		mesh_def.meshData = mesh;
		mesh_def.transform = b3Transform_identity;
		mesh_def.scale = { 1, 1, 1 };
		mesh_def.materials = &material;
		mesh_def.materialCount = 1;
	}
	b3CompoundDef cd = {};
	cd.hulls = hull_defs.empty() ? nullptr : hull_defs.data();
	cd.hullCount = int( bc + hc );
	cd.meshes = mesh_defs.empty() ? nullptr : mesh_defs.data();
	cd.meshCount = int( mc );
	b3CompoundData* compound = temporary.compound = b3CreateCompound( &cd );
	if ( !compound )
		return none;
	Ref<Box3DBakedCollision> result;
	result.instantiate();
	const bool ok =
		result->assign_bytes( reinterpret_cast<const uint8_t*>( compound ), compound->byteCount );
	return ok ? result : none;
}

Ref<Box3DBakedCollision> Box3DBakedCollision::from_bytes( const PackedByteArray& bytes )
{
	Ref<Box3DBakedCollision> result;
	if ( bytes.size() < int64_t( sizeof( b3CompoundData ) ) || bytes.size() > MAX_BYTES )
		return result;
	result.instantiate();
	if ( !result->assign_bytes( bytes.ptr(), bytes.size() ) )
		result.unref();
	return result;
}
PackedByteArray Box3DBakedCollision::get_bytes() const
{
	PackedByteArray bytes;
	bytes.resize( byte_count );
	if ( byte_count )
		std::memcpy( bytes.ptrw(), storage.data(), size_t( byte_count ) );
	return bytes;
}
Dictionary Box3DBakedCollision::get_info() const
{
	Dictionary out;
	const auto* c = get_compound();
	if ( !c )
		return out;
	out["byte_count"] = byte_count;
	out["mesh_count"] = c->meshCount;
	out["hull_count"] = c->hullCount;
	out["primitive_count"] = c->hullCount;
	int64_t triangle_count = 0;
	for ( int i = 0; i < c->meshCount; ++i )
		triangle_count += b3GetCompoundMesh( c, i ).meshData->triangleCount;
	out["triangle_count"] = triangle_count;
	out["shared_hull_count"] = c->sharedHullCount;
	out["shared_mesh_count"] = c->sharedMeshCount;
	out["material_count"] = c->materialCount;
	out["friction"] = b3GetCompoundMaterials( c )[0].friction;
	out["compound_version"] = String::num_uint64( B3_COMPOUND_VERSION, 16 );
	out["mesh_version"] = String::num_uint64( B3_MESH_VERSION, 16 );
	out["hull_version"] = String::num_uint64( B3_HULL_VERSION, 16 );
	b3Version v = b3GetVersion();
	out["native_version"] = String::num_int64( v.major ) + "." + String::num_int64( v.minor ) +
							"." + String::num_int64( v.revision );
	out["binding_version"] = "0.6.0+offline-baked-v2";
	out["speculative_distance"] = B3_SPECULATIVE_DISTANCE;
	out["mesh_weld_tolerance"] = .001;
	return out;
}
Array Box3DBakedCollision::get_debug_geometry() const
{
	Array out;
	const auto* compound = get_compound();
	if ( !compound )
		return out;
	// Only read owned cooked data. Return independent Godot arrays, never views
	// into storage, so a debug overlay can outlive this resource or a body.
	auto span = []( int offset, int count, size_t stride, int header, int bytes ) {
		return offset >= header && count >= 0 && offset <= bytes &&
			   int64_t( count ) * int64_t( stride ) <= int64_t( bytes ) - offset;
	};
	auto transform = []( const b3Transform& xf ) {
		return Transform3D( Basis( to_gd( xf.q ) ), to_gd( xf.p ) );
	};
	for ( int i = 0; i < compound->meshCount; ++i )
	{
		const b3CompoundMesh instance = b3GetCompoundMesh( compound, i );
		const b3MeshData* mesh = instance.meshData;
		if ( !span( mesh->vertexOffset, mesh->vertexCount, sizeof( b3Vec3 ), sizeof( b3MeshData ),
					mesh->byteCount ) ||
			 !span( mesh->triangleOffset, mesh->triangleCount, sizeof( b3MeshTriangle ),
					sizeof( b3MeshData ), mesh->byteCount ) )
			return Array();
		const b3Vec3* vertices = b3GetMeshVertices( mesh );
		const b3MeshTriangle* triangles = b3GetMeshTriangles( mesh );
		PackedVector3Array faces;
		faces.resize( int64_t( mesh->triangleCount ) * 3 );
		for ( int t = 0; t < mesh->triangleCount; ++t )
		{
			const auto& triangle = triangles[t];
			const int indices[] = { triangle.index1, triangle.index3, triangle.index2 };
			for ( int k = 0; k < 3; ++k )
			{
				if ( indices[k] < 0 || indices[k] >= mesh->vertexCount )
					return Array();
				faces.set( int64_t( t ) * 3 + k, to_gd( vertices[indices[k]] ) );
			}
		}
		Transform3D xf = transform( instance.transform );
		// Local mesh scale precedes the rigid instance transform.
		xf.basis = xf.basis.scaled_local( to_gd( instance.scale ) );
		Dictionary entry;
		entry["faces"] = faces;
		entry["transform"] = xf;
		entry["kind"] = "mesh";
		out.append( entry );
	}
	for ( int i = 0; i < compound->hullCount; ++i )
	{
		const b3CompoundHull instance = b3GetCompoundHull( compound, i );
		const b3HullData* hull = instance.hull;
		if ( !span( hull->pointOffset, hull->vertexCount, sizeof( b3Vec3 ), sizeof( b3HullData ),
					hull->byteCount ) ||
			 !span( hull->edgeOffset, hull->edgeCount, sizeof( b3HullHalfEdge ),
					sizeof( b3HullData ), hull->byteCount ) ||
			 !span( hull->faceOffset, hull->faceCount, sizeof( b3HullFace ), sizeof( b3HullData ),
					hull->byteCount ) )
			return Array();
		const b3Vec3* points = b3GetHullPoints( hull );
		const b3HullHalfEdge* edges = b3GetHullEdges( hull );
		const b3HullFace* faces = b3GetHullFaces( hull );
		PackedVector3Array triangles;
		for ( int f = 0; f < hull->faceCount; ++f )
		{
			const int first = faces[f].edge;
			int e = first;
			std::vector<int> polygon;
			do
			{
				if ( e < 0 || e >= hull->edgeCount || edges[e].origin >= hull->vertexCount ||
					 edges[e].face != f || int( polygon.size() ) >= hull->edgeCount )
					return Array();
				polygon.push_back( edges[e].origin );
				e = edges[e].next;
			}
			while ( e != first );
			if ( polygon.size() < 3 )
				return Array();
			// Native faces are CCW. Godot front faces use clockwise winding.
			for ( size_t k = 1; k + 1 < polygon.size(); ++k )
			{
				triangles.append( to_gd( points[polygon[0]] ) );
				triangles.append( to_gd( points[polygon[k + 1]] ) );
				triangles.append( to_gd( points[polygon[k]] ) );
			}
		}
		Dictionary entry;
		entry["faces"] = triangles;
		entry["transform"] = transform( instance.transform );
		entry["kind"] = "hull";
		out.append( entry );
	}
	return out;
}
void Box3DBakedCollision::_bind_methods()
{
	ClassDB::bind_static_method(
		"Box3DBakedCollision",
		D_METHOD( "bake", "vertices", "indices", "boxes", "friction", "hulls" ),
		&Box3DBakedCollision::bake, DEFVAL( Array() ), DEFVAL( .05 ), DEFVAL( Array() ) );
	ClassDB::bind_static_method(
		"Box3DBakedCollision", D_METHOD( "bake_meshes", "meshes", "boxes", "friction", "hulls" ),
		&Box3DBakedCollision::bake_meshes, DEFVAL( Array() ), DEFVAL( .05 ), DEFVAL( Array() ) );
	ClassDB::bind_static_method( "Box3DBakedCollision", D_METHOD( "from_bytes", "bytes" ),
								 &Box3DBakedCollision::from_bytes );
	ClassDB::bind_method( D_METHOD( "get_bytes" ), &Box3DBakedCollision::get_bytes );
	ClassDB::bind_method( D_METHOD( "get_info" ), &Box3DBakedCollision::get_info );
	ClassDB::bind_method( D_METHOD( "get_debug_geometry" ),
						  &Box3DBakedCollision::get_debug_geometry );
}
