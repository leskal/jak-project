#include "Tfrag3Data.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/simd_util.h"

namespace tfrag3 {

void PackedTimeOfDay::serialize(Serializer& ser) {
  ser.from_pod_vector(&data);
  ser.from_ptr(&color_count);
}

void PackedTieVertices::serialize(Serializer& ser) {
  ser.from_pod_vector(&color_indices);
  ser.from_pod_vector(&matrices);
  ser.from_pod_vector(&matrix_groups);
  ser.from_pod_vector(&vertices);
}

void PackedShrubVertices::serialize(Serializer& ser) {
  ser.from_pod_vector(&matrices);
  ser.from_pod_vector(&instance_groups);
  ser.from_pod_vector(&vertices);
  ser.from_ptr(&total_vertex_count);
}

void StripDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_pod_vector(&runs);
  ser.from_pod_vector(&plain_indices);
  ser.from_pod_vector(&vis_groups);
  ser.from_ptr(&num_triangles);
}

void ShrubDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_ptr(&num_triangles);
  ser.from_ptr(&first_index_index);
  ser.from_ptr(&num_indices);
  ser.from_ptr(&proto_idx);
}

void InstancedStripDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_pod_vector(&vertex_index_stream);
  ser.from_pod_vector(&instance_groups);
  ser.from_ptr(&num_triangles);
}

void TieWindInstance::serialize(Serializer& ser) {
  ser.from_ptr(&matrix);
  ser.from_ptr(&wind_idx);
  ser.from_ptr(&stiffness);
}

void TfragTree::serialize(Serializer& ser) {
  ser.from_ptr(&kind);

  if (ser.is_saving()) {
    ser.save<size_t>(draws.size());
  } else {
    draws.resize(ser.load<size_t>());
  }
  for (auto& draw : draws) {
    draw.serialize(ser);
  }

  ser.from_pod_vector(&packed_vertices.vertices);
  ser.from_pod_vector(&packed_vertices.cluster_origins);
  colors.serialize(ser);
  bvh.serialize(ser);
  ser.from_ptr(&use_strips);
}

math::Vector3f vopmula(math::Vector3f a, math::Vector3f b) {
  return math::Vector3f(a.y() * b.z(), a.z() * b.x(), a.x() * b.y());
}

math::Vector3f vopmsub(math::Vector3f acc, math::Vector3f a, math::Vector3f b) {
  return acc - vopmula(a, b);
}

/*!
 * Compute the normal transformation for a TIE from the TIE matrix. This will return properly scaled
 * normals.
 */
std::array<math::Vector3f, 3> tie_normal_transform_v2(const std::array<math::Vector4f, 4>& m) {
  // let:
  // vf10, vf11, vf12, vf13 be the input matrix m
  std::array<math::Vector3f, 3> result;
  auto& vf10 = m[0];
  auto& vf11 = m[1];
  // auto& vf12 = m[2];

  //  lui t6, 16256
  //  mtc1 f1, t6 ;; 1.0
  //
  //  qmfc2.i s1, vf10
  //  mtc1 f12, s1
  float f12 = vf10.x();
  //  dsra32 s2, s1, 0
  //  mtc1 f13, s2
  float f13 = vf10.y();
  //  pextuw s2, r0, s2
  //  mtc1 f14, s2
  float f14 = vf10.z();
  //  mula.s f12, f12
  //  madda.s f13, f13
  //  madd.s f15, f14, f14
  float f15 = f12 * f12 + f13 * f13 + f14 * f14;
  float scale = 1.f / sqrtf(f15);
  //  rsqrt.s f15, f1, f15
  //  mfc1 s1, f15
  //  qmtc2.i vf14, s1
  //  vmulx.xyz vf16, vf10, vf14

  // vmulx.xyz vf16, vf10, vf14
  math::Vector3f vf16 = vf10.xyz() * scale;

  // vopmula.xyz acc, vf11, vf16
  math::Vector3f acc = vopmula(vf11.xyz(), vf16);

  // vopmsub.xyz vf17, vf16, vf11
  math::Vector3f vf17 = vopmsub(acc, vf16, vf11.xyz());

  // vopmula.xyz acc, vf16, vf17
  acc = vopmula(vf16, vf17);

  // vopmsub.xyz vf17, vf17, vf16
  vf17 = vopmsub(acc, vf17, vf16);

  // vmul.xyz vf14, vf17, vf17
  math::Vector3f vf14 = vf17.elementwise_multiply(vf17);

  // vmulax.w acc, vf0, vf14
  // vmadday.w acc, vf0, vf14
  // vmaddz.w vf14, vf0, vf14
  float sum = vf14.x() + vf14.y() + vf14.z();

  // vrsqrt Q, vf0.w, vf14.w
  float Q = 1.f / std::sqrt(sum);

  // vmulax.xyzw acc, vf24, vf16
  // vmadday.xyzw acc, vf25, vf16
  // vmaddz.xyzw vf10, vf26, vf16
  // vf10 = vf16; // assume cam is identity here.
  result[0] = vf16;

  // vwaitq
  // vmulq.xyz vf17, vf17, Q
  vf17 *= Q;

  // vopmula.xyz acc, vf16, vf17
  acc = vopmula(vf16, vf17);
  // vopmsub.xyz vf18, vf17, vf16
  math::Vector3f vf18 = vopmsub(acc, vf17, vf16);

  // vmulax.xyzw acc, vf24, vf17
  // vmadday.xyzw acc, vf25, vf17
  // vmaddz.xyzw vf11, vf26, vf17
  result[1] = vf17;

  // vmulax.xyzw acc, vf24, vf18
  // vmadday.xyzw acc, vf25, vf18
  // vmaddz.xyzw vf12, vf26, vf18
  result[2] = vf18;

  return result;
  //
  // sqc2 vf10, -112(t8)
  // sqc2 vf11, -96(t8)
  // sqc2 vf12, -80(t8)
}

s16 saturate_for_s10(s16 s10) {
  // our error should be 1 or less as an s8, or 4 as a s10.
  ASSERT(s10 >= -520 && s10 < 520);
  if (s10 < -512) {
    return -512;
  }
  if (s10 > 511) {
    return 511;
  }
  return s10;
}

u32 pack_to_gl_normal(s16 nx, s16 ny, s16 nz) {
  return (saturate_for_s10(nx) & 0x3ff) | ((saturate_for_s10(ny) & 0x3ff) << 10) |
         ((saturate_for_s10(nz) & 0x3ff) << 20);
}

/*!
 * Unpack tie normal by transforming and converting to s16 for OpenGL.
 */
u32 unpack_tie_normal(const std::array<math::Vector3f, 3>& mat, s8 nx, s8 ny, s8 nz) {
  // rotate the normal
  math::Vector3f nrm = math::Vector3f::zero();
  nrm += mat[0] * nx;
  nrm += mat[1] * ny;
  nrm += mat[2] * nz;

  // game used signed 8-bit normals, but OpenGL uses signed 10-bit
  // multiply by 2^2 = 4
  nrm *= 4;

  auto as_int = nrm.cast<s16>();

  return pack_to_gl_normal(as_int.x(), as_int.y(), as_int.z());
}

/*
void tie_normal_v3(__m128* out, const std::array<math::Vector4f, 4>& in) {
  math::Vector3f x_row = in[0].xyz();
  math::Vector3f y_row = in[1].xyz();
  math::Vector3f z_row = in[2].xyz();

  x_row.normalize();
  y_row = x_row.cross(y_row.cross(x_row)).normalized();
  z_row = x_row.cross(y_row);

  out[0] = _mm_setr_ps(x_row[0], x_row[1], x_row[2], 0);
  out[1] = _mm_setr_ps(y_row[0], y_row[1], y_row[2], 0);
  out[2] = _mm_setr_ps(z_row[0], z_row[1], z_row[2], 0);
}
 */

namespace {
/*!
 * (AI-assisted) Relighting mod.
 * Level geometry (tfrag, and tie without normals) has no normals in the game data, because the
 * lighting was baked into vertex colors. Compute smooth normals from the triangles so the
 * renderer can light it. Only vertices with nor == 0 are changed.
 * Vertices at the same position are "welded" so normals are smooth across texture seams.
 * Triangle winding in the game data is not reliable, so normals are added up with a consistent
 * sign; the shader flips them to face the camera.
 */
std::vector<u32> smooth_normals_packed(const std::vector<math::Vector3f>& verts,
                                       const std::vector<u32>& indices,
                                       bool strips) {
  struct Key {
    s64 x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<s64>()(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
    }
  };
  // (AI-assisted) weld points that are within ~1 cm (40 game units) of each other into one "slot".
  // The level is stored in many pieces, and points on the border of two pieces are often a few
  // mm apart, so look in the neighbouring grid cells too.
  constexpr float kWeld = 40.f;
  std::unordered_map<Key, std::vector<u32>, KeyHash> cell_slots;
  cell_slots.reserve(verts.size());
  std::vector<math::Vector3f> slot_pos;
  std::vector<u32> slot_of(verts.size());
  for (size_t i = 0; i < verts.size(); i++) {
    const auto& p = verts[i];
    const s64 cx = (s64)std::floor(p.x() / kWeld);
    const s64 cy = (s64)std::floor(p.y() / kWeld);
    const s64 cz = (s64)std::floor(p.z() / kWeld);
    s64 found = -1;
    for (int dx = -1; dx <= 1 && found < 0; dx++) {
      for (int dy = -1; dy <= 1 && found < 0; dy++) {
        for (int dz = -1; dz <= 1 && found < 0; dz++) {
          auto it = cell_slots.find(Key{cx + dx, cy + dy, cz + dz});
          if (it == cell_slots.end()) {
            continue;
          }
          for (u32 sl : it->second) {
            if ((slot_pos[sl] - p).squared_length() <= kWeld * kWeld) {
              found = sl;
              break;
            }
          }
        }
      }
    }
    if (found < 0) {
      found = (s64)slot_pos.size();
      slot_pos.push_back(p);
      cell_slots[Key{cx, cy, cz}].push_back((u32)found);
    }
    slot_of[i] = (u32)found;
  }
  const size_t num_slots = slot_pos.size();

  // Crease-aware smoothing. Each point first looks at the triangles it belongs to itself ("own"
  // direction). Then it averages in the triangles of all points at the same position, but only
  // those facing within ~78 degrees of its own direction. So a cliff wall doesn't average with
  // the grass top above it (sharp edge), while rounded rocks and hills stay smooth.
  std::vector<math::Vector3f> face_n;  // per triangle, length = 2 * area
  std::vector<std::vector<u32>> slot_faces(num_slots);
  std::vector<std::vector<u32>> slot_adj(num_slots);  // neighbouring slots (along triangle edges)
  std::vector<math::Vector3f> own(verts.size(), math::Vector3f::zero());

  auto add_tri = [&](u32 a, u32 b, u32 c) {
    if (a == b || b == c || a == c || a >= verts.size() || b >= verts.size() ||
        c >= verts.size()) {
      return;
    }
    const math::Vector3f& pa = verts[a];
    const math::Vector3f& pb = verts[b];
    const math::Vector3f& pc = verts[c];
    math::Vector3f fn = (pb - pa).cross(pc - pa);  // length = 2 * area: big faces count more
    if (!(fn.squared_length() > 0.f)) {
      return;
    }
    const u32 face_idx = (u32)face_n.size();
    face_n.push_back(fn);
    for (u32 v : {a, b, c}) {
      auto& n = own[v];
      n += (n.dot(fn) < 0.f) ? fn * -1.f : fn;
      auto& list = slot_faces[slot_of[v]];
      if (list.empty() || list.back() != face_idx) {
        list.push_back(face_idx);
      }
    }
    const u32 sa = slot_of[a], sb = slot_of[b], sc = slot_of[c];
    if (sa != sb) {
      slot_adj[sa].push_back(sb);
      slot_adj[sb].push_back(sa);
    }
    if (sb != sc) {
      slot_adj[sb].push_back(sc);
      slot_adj[sc].push_back(sb);
    }
    if (sa != sc) {
      slot_adj[sa].push_back(sc);
      slot_adj[sc].push_back(sa);
    }
  };

  if (strips) {
    size_t run_start = 0;
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] == UINT32_MAX) {
        run_start = i + 1;
        continue;
      }
      size_t k = i - run_start;
      if (k >= 2) {
        add_tri(indices[i - 2], indices[i - 1], indices[i]);
      }
    }
  } else {
    u32 tri[3];
    int n = 0;
    for (u32 idx : indices) {
      if (idx == UINT32_MAX) {
        n = 0;
        continue;
      }
      tri[n++] = idx;
      if (n == 3) {
        add_tri(tri[0], tri[1], tri[2]);
        n = 0;
      }
    }
  }
  for (auto& adj : slot_adj) {
    std::sort(adj.begin(), adj.end());
    adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
  }

  constexpr float kCreaseCos = 0.2f;  // cos(~78 degrees): only real sharp edges (~90) split
  std::vector<math::Vector3f> vn(verts.size(), math::Vector3f::zero());
  for (size_t i = 0; i < verts.size(); i++) {
    math::Vector3f ref = own[i];
    float ref_len = ref.length();
    if (!(ref_len > 0.f)) {
      continue;  // not used by any triangle
    }
    ref = ref * (1.f / ref_len);
    math::Vector3f n = math::Vector3f::zero();
    for (u32 fi : slot_faces[slot_of[i]]) {
      const auto& fn = face_n[fi];
      const float fl = fn.length();
      const float d = fn.dot(ref) / fl;
      if (std::abs(d) >= kCreaseCos) {
        n += d < 0.f ? fn * -1.f : fn;
      }
    }
    float len = n.length();
    vn[i] = len > 0.f ? n * (1.f / len) : ref;
  }

  // Wider smoothing for the ground: the terrain is low detail, made of big flat areas, so normals
  // that are only averaged at the corners still look like flat patches. Points that are not on a
  // sharp edge ("smooth" slots) are blended with their neighbours a few times, so the normal
  // changes gradually over several triangles. Sharp edges are left alone.
  std::vector<math::Vector3f> sn(num_slots, math::Vector3f::zero());
  std::vector<u8> smooth_slot(num_slots, 0);
  for (size_t sl = 0; sl < num_slots; sl++) {
    math::Vector3f sum = math::Vector3f::zero();
    for (u32 fi : slot_faces[sl]) {
      const auto& fn = face_n[fi];
      sum += (sum.dot(fn) < 0.f) ? fn * -1.f : fn;
    }
    const float l = sum.length();
    if (!(l > 0.f)) {
      continue;
    }
    sum = sum * (1.f / l);
    bool smooth = true;
    for (u32 fi : slot_faces[sl]) {
      const auto& fn = face_n[fi];
      if (std::abs(fn.dot(sum)) / fn.length() < 0.5f) {  // a face more than 60 deg off: an edge
        smooth = false;
        break;
      }
    }
    sn[sl] = sum;
    smooth_slot[sl] = smooth ? 1 : 0;
  }
  constexpr int kRelaxIterations = 8;
  std::vector<math::Vector3f> next(num_slots);
  for (int it = 0; it < kRelaxIterations; it++) {
    for (size_t sl = 0; sl < num_slots; sl++) {
      next[sl] = sn[sl];
      if (!smooth_slot[sl]) {
        continue;
      }
      math::Vector3f sum = sn[sl];
      for (u32 nb : slot_adj[sl]) {
        if (!smooth_slot[nb]) {
          continue;
        }
        const float d = sn[nb].dot(sn[sl]);
        if (std::abs(d) >= 0.5f) {
          sum += d < 0.f ? sn[nb] * -1.f : sn[nb];
        }
      }
      const float l = sum.length();
      if (l > 0.f) {
        next[sl] = sum * (1.f / l);
      }
    }
    std::swap(sn, next);
  }

  // (AI-assisted) blend across seams. Some ground pieces don't share points with their neighbours
  // (separate pieces, or a corner of one piece in the middle of the other's edge), so the
  // smoothing above can't cross from one to the other. Blend each smooth point with all smooth
  // points within 2.5 m that face a similar direction, closer ones counting more.
  for (int pass = 0; pass < 2; pass++) {
    constexpr float kRadius = 4.f * 4096.f;
    std::unordered_map<Key, std::vector<u32>, KeyHash> grid;
    grid.reserve(num_slots);
    auto cell_of = [&](const math::Vector3f& p) {
      return Key{(s64)std::floor(p.x() / kRadius), (s64)std::floor(p.y() / kRadius),
                 (s64)std::floor(p.z() / kRadius)};
    };
    for (size_t sl = 0; sl < num_slots; sl++) {
      if (smooth_slot[sl] && sn[sl].squared_length() > 0.f) {
        grid[cell_of(slot_pos[sl])].push_back((u32)sl);
      }
    }
    for (size_t sl = 0; sl < num_slots; sl++) {
      next[sl] = sn[sl];
      if (!smooth_slot[sl] || !(sn[sl].squared_length() > 0.f)) {
        continue;
      }
      const Key c = cell_of(slot_pos[sl]);
      math::Vector3f sum = sn[sl];
      int looked = 0;  // limit the work in very detailed areas (load time)
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dz = -1; dz <= 1; dz++) {
            auto it = grid.find(Key{c.x + dx, c.y + dy, c.z + dz});
            if (it == grid.end()) {
              continue;
            }
            for (u32 nb : it->second) {
              if (nb == sl) {
                continue;
              }
              if (++looked > 256) {
                break;
              }
              const float d = (slot_pos[nb] - slot_pos[sl]).length();
              if (d >= kRadius) {
                continue;
              }
              const float a = sn[nb].dot(sn[sl]);
              if (std::abs(a) < 0.7f) {
                continue;  // faces a different way: a real edge, not a seam
              }
              const float w = 1.f - d / kRadius;
              sum += (a < 0.f ? sn[nb] * -1.f : sn[nb]) * w;
            }
          }
        }
      }
      const float l = sum.length();
      if (l > 0.f) {
        next[sl] = sum * (1.f / l);
      }
    }
    std::swap(sn, next);
  }

  // (AI-assisted) make the normals point to the same side across the surface. The game's
  // triangle winding is unreliable, so so far each point's normal could point up or down. The
  // shader turns normals towards the camera, but only after they are blended across a triangle:
  // if one corner points up and another down, the blend goes through zero in the middle (flat
  // looking lines). Walk over the surface from neighbour to neighbour and flip each normal to
  // agree with the one it was reached from; each connected surface starts pointing up.
  {
    std::vector<u8> visited(num_slots, 0);
    std::vector<u32> queue;
    for (size_t start = 0; start < num_slots; start++) {
      if (visited[start] || !(sn[start].squared_length() > 0.f)) {
        continue;
      }
      if (sn[start].y() < 0.f) {
        sn[start] = sn[start] * -1.f;
      }
      visited[start] = 1;
      queue.clear();
      queue.push_back((u32)start);
      for (size_t qi = 0; qi < queue.size(); qi++) {
        const u32 cur = queue[qi];
        for (u32 nb : slot_adj[cur]) {
          if (visited[nb] || !(sn[nb].squared_length() > 0.f)) {
            continue;
          }
          if (sn[nb].dot(sn[cur]) < 0.f) {
            sn[nb] = sn[nb] * -1.f;
          }
          visited[nb] = 1;
          queue.push_back(nb);
        }
      }
    }
  }

  std::vector<u32> result(verts.size(), 0);
  for (size_t i = 0; i < verts.size(); i++) {
    math::Vector3f n = vn[i];
    if (!(n.squared_length() > 0.f)) {
      continue;
    }
    const u32 sl = slot_of[i];
    const math::Vector3f& r = sn[sl];
    if (smooth_slot[sl] && std::abs(r.dot(n)) > 0.5f) {
      n = r;  // smoothed, and pointing to the same side as its neighbours
    } else if (r.dot(n) < 0.f) {
      n = n * -1.f;  // edge point: keep its own direction, but on the same side
    }
    result[i] = pack_to_gl_normal((s16)std::lround(n.x() * 511.f), (s16)std::lround(n.y() * 511.f),
                                  (s16)std::lround(n.z() * 511.f));
  }
  return result;
}

// (AI-assisted) a usable normal? Some game data has tiny garbage normals (almost zero length),
// which shade as flat strips: treat those as missing too.
bool normal_is_missing(u32 nor) {
  if (nor == 0) {
    return true;
  }
  auto field = [&](int shift) {
    s32 v = (s32)((nor >> shift) & 1023);
    if (v & 512) {
      v -= 1024;  // sign extend 10 bits
    }
    return (float)v / 511.f;
  };
  const float x = field(0), y = field(10), z = field(20);
  return x * x + y * y + z * z < 0.04f;  // shorter than 0.2 (real game normals are ~0.5..1)
}

void compute_missing_normals(std::vector<PreloadedVertex>& verts,
                             const std::vector<u32>& indices,
                             bool strips) {
  bool any_missing = false;
  for (const auto& v : verts) {
    if (normal_is_missing(v.nor)) {
      any_missing = true;
      break;
    }
  }
  if (!any_missing) {
    return;
  }
  std::vector<math::Vector3f> pos;
  pos.reserve(verts.size());
  for (const auto& v : verts) {
    pos.emplace_back(v.x, v.y, v.z);
  }
  auto packed = smooth_normals_packed(pos, indices, strips);
  for (size_t i = 0; i < verts.size(); i++) {
    if (normal_is_missing(verts[i].nor)) {
      verts[i].nor = packed[i];
    }
  }
}
}  // namespace

void TieTree::unpack() {
  unpacked.vertices.resize(packed_vertices.color_indices.size());
  size_t i = 0;
  for (const auto& grp : packed_vertices.matrix_groups) {
    if (grp.matrix_idx == -1) {
      for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
        auto& vtx = unpacked.vertices[i];
        vtx.color_index = packed_vertices.color_indices[i];
        const auto& proto_vtx = packed_vertices.vertices[src_idx];
        vtx.x = proto_vtx.x;
        vtx.y = proto_vtx.y;
        vtx.z = proto_vtx.z;
        vtx.s = proto_vtx.s;
        vtx.t = proto_vtx.t;
        vtx.nor = pack_to_gl_normal(proto_vtx.nx << 1, proto_vtx.ny << 1, proto_vtx.nz << 1);
        vtx.r = proto_vtx.r;
        vtx.g = proto_vtx.g;
        vtx.b = proto_vtx.b;
        vtx.a = proto_vtx.a;
        i++;
      }
    } else {
      const auto& mat = packed_vertices.matrices[grp.matrix_idx];

      __m128 mat0 = _mm_loadu_ps(mat[0].data());
      __m128 mat1 = _mm_loadu_ps(mat[1].data());
      __m128 mat2 = _mm_loadu_ps(mat[2].data());
      __m128 mat3 = _mm_loadu_ps(mat[3].data());

      if (grp.has_normals) {
        auto nmat = tie_normal_transform_v2(mat);
        for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
          auto& vtx = unpacked.vertices[i];
          vtx.color_index = packed_vertices.color_indices[i];
          const auto& proto_vtx = packed_vertices.vertices[src_idx];
          // auto temp = mat[0] * proto_vtx.x + mat[1] * proto_vtx.y + mat[2] * proto_vtx.z +
          // mat[3];
          __m128 transformed = mat3;
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.x), mat0));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.y), mat1));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.z), mat2));
          _mm_storeu_ps(&vtx.x, transformed);
          vtx.s = proto_vtx.s;
          vtx.t = proto_vtx.t;
          vtx.nor = unpack_tie_normal(nmat, proto_vtx.nx, proto_vtx.ny, proto_vtx.nz);
          vtx.r = proto_vtx.r;
          vtx.g = proto_vtx.g;
          vtx.b = proto_vtx.b;
          vtx.a = proto_vtx.a;
          i++;
        }
      } else {
        for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
          auto& vtx = unpacked.vertices[i];
          vtx.color_index = packed_vertices.color_indices[i];
          const auto& proto_vtx = packed_vertices.vertices[src_idx];
          __m128 transformed = mat3;
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.x), mat0));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.y), mat1));
          transformed = _mm_add_ps(transformed, _mm_mul_ps(_mm_set1_ps(proto_vtx.z), mat2));
          _mm_storeu_ps(&vtx.x, transformed);
          vtx.s = proto_vtx.s;
          vtx.t = proto_vtx.t;
          vtx.nor = 0;
          vtx.r = proto_vtx.r;
          vtx.g = proto_vtx.g;
          vtx.b = proto_vtx.b;
          vtx.a = proto_vtx.a;
          i++;
        }
      }
    }
  }

  for (auto& draw : static_draws) {
    draw.unpacked.idx_of_first_idx_in_full_buffer = unpacked.indices.size();
    // indices can come from either runs or already in plain indices.
    for (auto& run : draw.runs) {
      for (u32 ri = 0; ri < run.length; ri++) {
        unpacked.indices.push_back(run.vertex0 + ri);
      }
      unpacked.indices.push_back(UINT32_MAX);
    }
    unpacked.indices.insert(unpacked.indices.end(), draw.plain_indices.begin(),
                            draw.plain_indices.end());
  }
  // (AI-assisted) relighting: wind (swaying) protos are drawn with their own index lists
  {
    std::vector<u32> all_indices = unpacked.indices;
    for (const auto& wd : instanced_wind_draws) {
      all_indices.push_back(UINT32_MAX);
      all_indices.insert(all_indices.end(), wd.vertex_index_stream.begin(),
                         wd.vertex_index_stream.end());
    }
    compute_missing_normals(unpacked.vertices, all_indices, use_strips);
  }
}

void ShrubTree::unpack() {
  unpacked.vertices.resize(packed_vertices.total_vertex_count);
  size_t i = 0;

  for (const auto& grp : packed_vertices.instance_groups) {
    const auto& mat = packed_vertices.matrices[grp.matrix_idx];
    for (u32 src_idx = grp.start_vert; src_idx < grp.end_vert; src_idx++) {
      auto& vtx = unpacked.vertices[i];
      vtx.color_index = grp.color_index;
      const auto& proto_vtx = packed_vertices.vertices[src_idx];
      auto temp = mat[0] * proto_vtx.x + mat[1] * proto_vtx.y + mat[2] * proto_vtx.z + mat[3];
      vtx.x = temp.x();
      vtx.y = temp.y();
      vtx.z = temp.z();
      vtx.s = proto_vtx.s;
      vtx.t = proto_vtx.t;
      memcpy(vtx.rgba_base, proto_vtx.rgba, 3);
      i++;
    }
  }
  ASSERT(i == unpacked.vertices.size());

  // (AI-assisted) relighting: compute normals, stored in the unused pad0 field
  std::vector<math::Vector3f> pos;
  pos.reserve(unpacked.vertices.size());
  for (const auto& v : unpacked.vertices) {
    pos.emplace_back(v.x, v.y, v.z);
  }
  auto packed = smooth_normals_packed(pos, indices, true);
  for (size_t vi = 0; vi < unpacked.vertices.size(); vi++) {
    unpacked.vertices[vi].pad0 = packed[vi];
  }
}

void TfragTree::unpack() {
  unpacked.vertices.resize(packed_vertices.vertices.size());
  for (size_t i = 0; i < unpacked.vertices.size(); i++) {
    auto& o = unpacked.vertices[i];
    auto& in = packed_vertices.vertices[i];
    auto& cluster = packed_vertices.cluster_origins.at(in.cluster_idx);
    constexpr float kClusterSize = 4096 * 40;  // 100 in-game meters
    constexpr float kMasterOffset = 12000 * 4096;
    constexpr float rescale = kClusterSize / UINT16_MAX;
    float cx = -kMasterOffset + kClusterSize * cluster.x();
    float cy = -kMasterOffset + kClusterSize * cluster.y();
    float cz = -kMasterOffset + kClusterSize * cluster.z();
    o.x = cx + in.xoff * rescale;
    o.y = cy + in.yoff * rescale;
    o.z = cz + in.zoff * rescale;
    o.s = in.s / (1024.f);
    o.t = in.t / (1024.f);
    o.color_index = in.color_index;
  }

  for (auto& draw : draws) {
    draw.unpacked.idx_of_first_idx_in_full_buffer = unpacked.indices.size();
    for (auto& run : draw.runs) {
      for (u32 ri = 0; ri < run.length; ri++) {
        unpacked.indices.push_back(run.vertex0 + ri);
      }
      if (use_strips) {
        unpacked.indices.push_back(UINT32_MAX);
      }
    }
    unpacked.indices.insert(unpacked.indices.end(), draw.plain_indices.begin(),
                            draw.plain_indices.end());
  }
  compute_missing_normals(unpacked.vertices, unpacked.indices, use_strips);  // (AI-assisted)
}

void TieTree::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(static_draws.size());
  } else {
    static_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : static_draws) {
    draw.serialize(ser);
  }

  ser.from_ptr(&category_draw_indices);

  if (ser.is_saving()) {
    ser.save<size_t>(instanced_wind_draws.size());
  } else {
    instanced_wind_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : instanced_wind_draws) {
    draw.serialize(ser);
  }

  if (ser.is_saving()) {
    ser.save<size_t>(wind_instance_info.size());
  } else {
    wind_instance_info.resize(ser.load<size_t>());
  }
  for (auto& inst : wind_instance_info) {
    inst.serialize(ser);
  }

  packed_vertices.serialize(ser);
  colors.serialize(ser);
  bvh.serialize(ser);

  ser.from_ptr(&use_strips);

  ser.from_ptr(&has_per_proto_visibility_toggle);
  ser.from_string_vector(&proto_names);
}

void ShrubTree::serialize(Serializer& ser) {
  time_of_day_colors.serialize(ser);
  ser.from_pod_vector(&indices);
  packed_vertices.serialize(ser);
  if (ser.is_saving()) {
    ser.save<size_t>(static_draws.size());
  } else {
    static_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : static_draws) {
    draw.serialize(ser);
  }

  ser.from_ptr(&has_per_proto_visibility_toggle);
  ser.from_string_vector(&proto_names);
}

void HfragmentBucket::serialize(Serializer& ser) {
  ser.from_pod_vector(&corners);
  ser.from_ptr(&montage_table);
}

void Hfragment::serialize(Serializer& ser) {
  ser.from_pod_vector(&vertices);
  ser.from_pod_vector(&indices);
  ser.from_pod_vector(&corners);

  if (ser.is_saving()) {
    ser.save<size_t>(buckets.size());
  } else {
    buckets.resize(ser.load<size_t>());
  }
  for (auto& x : buckets) {
    x.serialize(ser);
  }
  time_of_day_colors.serialize(ser);
  ser.from_ptr(&wang_tree_tex_id);
  ser.from_ptr(&draw_mode);
  ser.from_ptr(&occlusion_offset);
}

void BVH::serialize(Serializer& ser) {
  ser.from_ptr(&first_leaf_node);
  ser.from_ptr(&last_leaf_node);
  ser.from_ptr(&first_root);
  ser.from_ptr(&num_roots);
  ser.from_ptr(&only_children);
  ser.from_pod_vector(&vis_nodes);
}

void Texture::serialize(Serializer& ser) {
  ser.from_ptr(&w);
  ser.from_ptr(&h);
  ser.from_ptr(&combo_id);
  ser.from_pod_vector(&data);
  ser.from_str(&debug_name);
  ser.from_str(&debug_tpage_name);
  ser.from_ptr(&load_to_pool);
}

void IndexTexture::serialize(Serializer& ser) {
  ser.from_ptr(&w);
  ser.from_ptr(&h);
  ser.from_ptr(&combo_id);
  ser.from_pod_vector(&index_data);
  ser.from_ptr(&color_table);
  ser.from_str(&name);
  ser.from_str(&tpage_name);
  ser.from_string_vector(&level_names);
}

void CollisionMesh::serialize(Serializer& ser) {
  ser.from_pod_vector(&vertices);
}

void MercDraw::serialize(Serializer& ser) {
  ser.from_ptr(&mode);
  ser.from_ptr(&tree_tex_id);
  ser.from_ptr(&eye_id);
  ser.from_ptr(&first_index);
  ser.from_ptr(&index_count);
  ser.from_ptr(&num_triangles);
  ser.from_ptr(&no_strip);
}

void Blerc::serialize(Serializer& ser) {
  ser.from_pod_vector(&float_data);
  ser.from_pod_vector(&int_data);
}

void MercModifiableDrawGroup::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(mod_draw.size());
  } else {
    mod_draw.resize(ser.load<size_t>());
  }
  for (auto& draw : mod_draw) {
    draw.serialize(ser);
  }

  if (ser.is_saving()) {
    ser.save<size_t>(fix_draw.size());
  } else {
    fix_draw.resize(ser.load<size_t>());
  }
  for (auto& draw : fix_draw) {
    draw.serialize(ser);
  }
  ser.from_pod_vector(&vertices);
  ser.from_pod_vector(&vertex_lump4_addr);
  ser.from_pod_vector(&fragment_mask);
  ser.from_ptr(&expect_vidx_end);

  blerc.serialize(ser);
}

void MercEffect::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(all_draws.size());
  } else {
    all_draws.resize(ser.load<size_t>());
  }
  for (auto& draw : all_draws) {
    draw.serialize(ser);
  }

  mod.serialize(ser);

  ser.from_ptr(&envmap_mode);
  ser.from_ptr(&envmap_texture);
  ser.from_ptr(&has_envmap);
  ser.from_ptr(&has_mod_draw);
}

void MercModel::serialize(Serializer& ser) {
  ser.from_str(&name);
  if (ser.is_saving()) {
    ser.save<size_t>(effects.size());
  } else {
    effects.resize(ser.load<size_t>());
  }
  for (auto& effect : effects) {
    effect.serialize(ser);
  }
  ser.from_ptr(&max_draws);
  ser.from_ptr(&max_bones);
  ser.from_ptr(&st_vif_add);
  ser.from_ptr(&xyz_scale);
  ser.from_ptr(&st_magic);
}

void MercModelGroup::serialize(Serializer& ser) {
  if (ser.is_saving()) {
    ser.save<size_t>(models.size());
  } else {
    models.resize(ser.load<size_t>());
  }
  for (auto& model : models) {
    model.serialize(ser);
  }

  ser.from_pod_vector(&indices);
  ser.from_pod_vector(&vertices);
}

void Level::serialize(Serializer& ser) {
  ser.from_ptr(&version);
  if (ser.is_loading() && version != TFRAG3_VERSION) {
    ASSERT_MSG(false, fmt::format("version mismatch when loading tfrag3 data. Got {}, expected {}, "
                                  "did you forget to re-decompile?",
                                  version, TFRAG3_VERSION));
  }

  ser.from_str(&level_name);

  if (ser.is_saving()) {
    ser.save<size_t>(textures.size());
  } else {
    textures.resize(ser.load<size_t>());
  }
  for (auto& tex : textures) {
    tex.serialize(ser);
  }

  if (ser.is_saving()) {
    ser.save<size_t>(index_textures.size());
  } else {
    index_textures.resize(ser.load<size_t>());
  }
  for (auto& tex : index_textures) {
    tex.serialize(ser);
  }

  for (int geom = 0; geom < 3; ++geom) {
    if (ser.is_saving()) {
      ser.save<size_t>(tfrag_trees[geom].size());
    } else {
      tfrag_trees[geom].resize(ser.load<size_t>());
    }
    for (auto& tree : tfrag_trees[geom]) {
      tree.serialize(ser);
    }
  }

  for (int geom = 0; geom < 4; ++geom) {
    if (ser.is_saving()) {
      ser.save<size_t>(tie_trees[geom].size());
    } else {
      tie_trees[geom].resize(ser.load<size_t>());
    }
    for (auto& tree : tie_trees[geom]) {
      tree.serialize(ser);
    }
  }

  if (ser.is_saving()) {
    ser.save<size_t>(shrub_trees.size());
  } else {
    shrub_trees.resize(ser.load<size_t>());
  }
  for (auto& tree : shrub_trees) {
    tree.serialize(ser);
  }

  hfrag.serialize(ser);

  collision.serialize(ser);
  merc_data.serialize(ser);

  ser.from_ptr(&version2);
  if (ser.is_loading() && version2 != TFRAG3_VERSION) {
    ASSERT_MSG(false, fmt::format(
                          "version mismatch when loading tfrag3 data (at end). Got {}, expected {}",
                          version2, TFRAG3_VERSION));
  }
}

void MercModifiableDrawGroup::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::MERC_MOD_VERT, sizeof(MercVertex) * vertices.size());
  tracker->add(MemoryUsageCategory::MERC_MOD_DRAW_1, sizeof(MercDraw) * fix_draw.size());
  tracker->add(MemoryUsageCategory::MERC_MOD_DRAW_2, sizeof(MercDraw) * mod_draw.size());
  tracker->add(MemoryUsageCategory::MERC_MOD_TABLE, sizeof(u16) * vertex_lump4_addr.size());
  tracker->add(MemoryUsageCategory::BLERC, sizeof(BlercFloatData) * blerc.float_data.size());
  tracker->add(MemoryUsageCategory::BLERC, sizeof(u32) * blerc.int_data.size());
}

void MercEffect::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::MERC_DRAW, sizeof(MercDraw) * all_draws.size());
  mod.memory_usage(tracker);
}

void MercModel::memory_usage(MemoryUsageTracker* tracker) const {
  for (auto& effect : effects) {
    effect.memory_usage(tracker);
  }
}

void MercModelGroup::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::MERC_VERT, sizeof(MercVertex) * vertices.size());
  tracker->add(MemoryUsageCategory::MERC_INDEX, sizeof(u32) * indices.size());
  for (auto& model : models) {
    model.memory_usage(tracker);
  }
}

void CollisionMesh::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::COLLISION, sizeof(Vertex) * vertices.size());
}

void PackedShrubVertices::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::SHRUB_VERT, 64 * matrices.size());
  tracker->add(MemoryUsageCategory::SHRUB_VERT, sizeof(InstanceGroup) * instance_groups.size());
  tracker->add(MemoryUsageCategory::SHRUB_VERT, sizeof(Vertex) * vertices.size());
}

void ShrubTree::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::SHRUB_TIME_OF_DAY, sizeof(u8) * time_of_day_colors.data.size());
  packed_vertices.memory_usage(tracker);
  tracker->add(MemoryUsageCategory::SHRUB_DRAW, sizeof(ShrubDraw) * static_draws.size());
  tracker->add(MemoryUsageCategory::SHRUB_IND, sizeof(u32) * indices.size());
}

void InstancedStripDraw::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TIE_INST_INDEX, sizeof(u32) * vertex_index_stream.size());
  tracker->add(MemoryUsageCategory::TIE_INST_VIS, sizeof(InstanceGroup) * instance_groups.size());
}

void PackedTieVertices::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TIE_CIDX, sizeof(u16) * color_indices.size());
  tracker->add(MemoryUsageCategory::TIE_MATRICES, 64 * matrices.size());
  tracker->add(MemoryUsageCategory::TIE_GRPS, sizeof(MatrixGroup) * matrix_groups.size());
  tracker->add(MemoryUsageCategory::TIE_VERTS, sizeof(Vertex) * vertices.size());
}

void TieTree::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TIE_BVH, sizeof(VisNode) * bvh.vis_nodes.size());
  for (auto& draw : static_draws) {
    tracker->add(MemoryUsageCategory::TIE_DEINST_INDEX,
                 draw.runs.size() * sizeof(StripDraw::VertexRun));
    tracker->add(MemoryUsageCategory::TIE_DEINST_INDEX, draw.plain_indices.size() * sizeof(u32));
    tracker->add(MemoryUsageCategory::TIE_DEINST_VIS,
                 draw.vis_groups.size() * sizeof(StripDraw::VisGroup));
  }
  packed_vertices.memory_usage(tracker);
  tracker->add(MemoryUsageCategory::TIE_TIME_OF_DAY, sizeof(u8) * colors.data.size());

  for (auto& draw : instanced_wind_draws) {
    draw.memory_usage(tracker);
  }
  tracker->add(MemoryUsageCategory::TIE_WIND_INSTANCE_INFO,
               sizeof(TieWindInstance) * wind_instance_info.size());
}

void PackedTfragVertices::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TFRAG_VERTS,
               sizeof(PackedTfragVertices::Vertex) * vertices.size());
  tracker->add(MemoryUsageCategory::TFRAG_CLUSTER,
               sizeof(math::Vector<u16, 3>) * cluster_origins.size());
}

void TfragTree::memory_usage(MemoryUsageTracker* tracker) const {
  for (auto& draw : draws) {
    tracker->add(MemoryUsageCategory::TFRAG_INDEX, draw.runs.size() * sizeof(StripDraw::VertexRun));
    tracker->add(MemoryUsageCategory::TFRAG_INDEX, draw.plain_indices.size() * sizeof(u32));
    tracker->add(MemoryUsageCategory::TFRAG_VIS,
                 draw.vis_groups.size() * sizeof(StripDraw::VisGroup));
  }
  packed_vertices.memory_usage(tracker);
  tracker->add(MemoryUsageCategory::TFRAG_TIME_OF_DAY, sizeof(u8) * colors.data.size());
  tracker->add(MemoryUsageCategory::TFRAG_BVH, sizeof(VisNode) * bvh.vis_nodes.size());
}

void Texture::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::TEXTURE, data.size() * sizeof(u32));
}

void IndexTexture::memory_usage(MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::SPECIAL_TEXTURE, index_data.size());
  tracker->add(MemoryUsageCategory::SPECIAL_TEXTURE, 256 * 4);  // clut
}

void Hfragment::memory_usage(tfrag3::MemoryUsageTracker* tracker) const {
  tracker->add(MemoryUsageCategory::HFRAG_VERTS, vertices.size() * sizeof(HfragmentVertex));
  tracker->add(MemoryUsageCategory::HFRAG_INDEX, indices.size() * sizeof(u32));
  tracker->add(MemoryUsageCategory::HFRAG_TIME_OF_DAY, time_of_day_colors.data.size() * sizeof(u8));
  tracker->add(MemoryUsageCategory::HFRAG_CORNERS, corners.size() * sizeof(HfragmentCorner));
}

void Level::memory_usage(MemoryUsageTracker* tracker) const {
  for (const auto& texture : textures) {
    texture.memory_usage(tracker);
  }
  for (const auto& texture : index_textures) {
    texture.memory_usage(tracker);
  }
  for (const auto& tftk : tfrag_trees) {
    for (const auto& tree : tftk) {
      tree.memory_usage(tracker);
    }
  }
  for (const auto& ttk : tie_trees) {
    for (const auto& tree : ttk) {
      tree.memory_usage(tracker);
    }
  }
  for (const auto& tree : shrub_trees) {
    tree.memory_usage(tracker);
  }
  hfrag.memory_usage(tracker);
  collision.memory_usage(tracker);
  merc_data.memory_usage(tracker);
}

void print_memory_usage(const tfrag3::Level& lev, int uncompressed_data_size) {
  int total_accounted = 0;
  MemoryUsageTracker mem_use;
  lev.memory_usage(&mem_use);

  std::vector<std::pair<std::string, int>> known_categories = {
      {"texture", mem_use.data[tfrag3::MemoryUsageCategory::TEXTURE]},
      {"special-texture", mem_use.data[tfrag3::MemoryUsageCategory::SPECIAL_TEXTURE]},
      {"tie-deinst-vis", mem_use.data[tfrag3::MemoryUsageCategory::TIE_DEINST_VIS]},
      {"tie-deinst-idx", mem_use.data[tfrag3::MemoryUsageCategory::TIE_DEINST_INDEX]},
      {"tie-inst-vis", mem_use.data[tfrag3::MemoryUsageCategory::TIE_INST_VIS]},
      {"tie-inst-idx", mem_use.data[tfrag3::MemoryUsageCategory::TIE_INST_INDEX]},
      {"tie-bvh", mem_use.data[tfrag3::MemoryUsageCategory::TIE_BVH]},
      {"tie-verts", mem_use.data[tfrag3::MemoryUsageCategory::TIE_VERTS]},
      {"tie-colors", mem_use.data[tfrag3::MemoryUsageCategory::TIE_TIME_OF_DAY]},
      {"tie-wind-inst-info", mem_use.data[tfrag3::MemoryUsageCategory::TIE_WIND_INSTANCE_INFO]},
      {"tie-cidx", mem_use.data[tfrag3::MemoryUsageCategory::TIE_CIDX]},
      {"tie-mats", mem_use.data[tfrag3::MemoryUsageCategory::TIE_MATRICES]},
      {"tie-grps", mem_use.data[tfrag3::MemoryUsageCategory::TIE_GRPS]},
      {"tfrag-vis", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_VIS]},
      {"tfrag-idx", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_INDEX]},
      {"tfrag-vert", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_VERTS]},
      {"tfrag-colors", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_TIME_OF_DAY]},
      {"tfrag-cluster", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_CLUSTER]},
      {"tfrag-bvh", mem_use.data[tfrag3::MemoryUsageCategory::TFRAG_BVH]},
      {"shrub-colors", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_TIME_OF_DAY]},
      {"shrub-vert", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_VERT]},
      {"shrub-ind", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_IND]},
      {"shrub-draw", mem_use.data[tfrag3::MemoryUsageCategory::SHRUB_DRAW]},
      {"collision", mem_use.data[tfrag3::MemoryUsageCategory::COLLISION]},
      {"merc-vert", mem_use.data[tfrag3::MemoryUsageCategory::MERC_VERT]},
      {"merc-idx", mem_use.data[tfrag3::MemoryUsageCategory::MERC_INDEX]},
      {"merc-draw", mem_use.data[tfrag3::MemoryUsageCategory::MERC_DRAW]},
      {"merc-mod-vert", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_VERT]},
      {"merc-mod-ind", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_IND]},
      {"merc-mod-table", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_TABLE]},
      {"merc-mod-draw-1", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_DRAW_1]},
      {"merc-mod-draw-2", mem_use.data[tfrag3::MemoryUsageCategory::MERC_MOD_DRAW_2]},
      {"blerc", mem_use.data[tfrag3::MemoryUsageCategory::BLERC]},
      {"hfrag-verts", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_VERTS]},
      {"hfrag-index", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_INDEX]},
      {"hfrag-time-of-day", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_TIME_OF_DAY]},
      {"hfrag-corners", mem_use.data[tfrag3::MemoryUsageCategory::HFRAG_CORNERS]}

  };
  for (auto& known : known_categories) {
    total_accounted += known.second;
  }

  known_categories.push_back({"unknown", uncompressed_data_size - total_accounted});

  std::sort(known_categories.begin(), known_categories.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  for (const auto& x : known_categories) {
    if (x.second) {
      lg::print("{:30s} : {:6d} kB {:3.1f}%\n", x.first, x.second / 1024,
                100.f * (float)x.second / uncompressed_data_size);
    }
  }
}

std::size_t PreloadedVertex::hash::operator()(const PreloadedVertex& v) const {
  return std::hash<float>()(v.x) ^ std::hash<float>()(v.y) ^ std::hash<float>()(v.z) ^
         std::hash<float>()(v.s) ^ std::hash<float>()(v.t) ^ std::hash<u16>()(v.color_index);
}

std::size_t PackedTieVertices::Vertex::hash::operator()(const Vertex& v) const {
  return std::hash<float>()(v.x) ^ std::hash<float>()(v.y) ^ std::hash<float>()(v.z) ^
         std::hash<float>()(v.r) ^ std::hash<float>()(v.g) ^ std::hash<float>()(v.b) ^
         std::hash<float>()(v.a) ^ std::hash<float>()(v.s) ^ std::hash<float>()(v.t) ^
         std::hash<float>()(v.nx) ^ std::hash<float>()(v.ny) ^ std::hash<float>()(v.nz);
}

}  // namespace tfrag3
