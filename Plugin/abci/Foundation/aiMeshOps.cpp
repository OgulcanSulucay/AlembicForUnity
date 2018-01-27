#include "pch.h"
#include "aiMeshOps.h"


namespace impl
{
    template<class Indices, class Counts>
    inline void BuildConnection(
        MeshConnectionInfo& connection, const Indices& indices, const Counts& counts, const IArray<float3>& vertices)
    {
        size_t num_points = vertices.size();
        size_t num_faces = counts.size();
        size_t num_indices = indices.size();

        connection.v2f_offsets.resize_discard(num_points);
        connection.v2f_faces.resize_discard(num_indices);
        connection.v2f_indices.resize_discard(num_indices);

        connection.v2f_counts.resize_zeroclear(num_points);
        {
            int ii = 0;
            for (size_t fi = 0; fi < num_faces; ++fi) {
                int c = counts[fi];
                for (int ci = 0; ci < c; ++ci) {
                    connection.v2f_counts[indices[ii + ci]]++;
                }
                ii += c;
            }

            int offset = 0;
            for (size_t i = 0; i < num_points; ++i) {
                connection.v2f_offsets[i] = offset;
                offset += connection.v2f_counts[i];
            }
        }

        connection.v2f_counts.zeroclear();
        {
            int i = 0;
            for (int fi = 0; fi < (int)num_faces; ++fi) {
                int c = counts[fi];
                for (int ci = 0; ci < c; ++ci) {
                    int vi = indices[i + ci];
                    int ti = connection.v2f_offsets[vi] + connection.v2f_counts[vi]++;
                    connection.v2f_faces[ti] = fi;
                    connection.v2f_indices[ti] = i + ci;
                }
                i += c;
            }
        }
    }
}

void MeshConnectionInfo::clear()
{
    v2f_counts.clear();
    v2f_offsets.clear();
    v2f_faces.clear();
    v2f_indices.clear();

    weld_map.clear();
    weld_counts.clear();
    weld_offsets.clear();
    weld_indices.clear();
}

void MeshConnectionInfo::buildConnection(
    const IArray<int>& indices, const IArray<int>& counts, const IArray<float3>& vertices)
{
    size_t num_points = vertices.size();
    size_t num_faces = counts.size();
    size_t num_indices = indices.size();

    v2f_offsets.resize_discard(num_points);
    v2f_faces.resize_discard(num_indices);
    v2f_indices.resize_discard(num_indices);

    v2f_counts.resize_zeroclear(num_points);
    {
        int ii = 0;
        for (size_t fi = 0; fi < num_faces; ++fi) {
            int c = counts[fi];
            for (int ci = 0; ci < c; ++ci) {
                v2f_counts[indices[ii + ci]]++;
            }
            ii += c;
        }

        int offset = 0;
        for (size_t i = 0; i < num_points; ++i) {
            v2f_offsets[i] = offset;
            offset += v2f_counts[i];
        }
    }

    v2f_counts.zeroclear();
    {
        int i = 0;
        for (int fi = 0; fi < (int)num_faces; ++fi) {
            int c = counts[fi];
            for (int ci = 0; ci < c; ++ci) {
                int vi = indices[i + ci];
                int ti = v2f_offsets[vi] + v2f_counts[vi]++;
                v2f_faces[ti] = fi;
                v2f_indices[ti] = i + ci;
            }
            i += c;
        }
    }
}


static inline int next_power_of_two(uint32_t v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v + (v == 0);
}

void MeshWelder::prepare(abcV3 * points, int count)
{
    m_points = points;
    m_count = count;

    m_hash_size = next_power_of_two(m_count);
    m_hash_table.resize_discard(m_hash_size + m_count);
    memset(m_hash_table.data(), NIL, m_hash_size * sizeof(int));

    m_remap.resize_discard(m_count);
}

const RawVector<int>& MeshWelder::getRemapTable() const
{
    return m_remap;
}



void MeshRefiner::triangulate(bool swap_faces)
{
    auto& dst = new_indices_triangulated;
    dst.resize(num_new_indices);

    const int i1 = swap_faces ? 2 : 1;
    const int i2 = swap_faces ? 1 : 2;
    size_t num_faces = counts.size();

    int n = 0;
    int i = 0;
    for (size_t fi = 0; fi < num_faces; ++fi) {
        int count = counts[fi];
        for (int ni = 0; ni < count - 2; ++ni) {
            dst[i + 0] = new_indices[n + 0];
            dst[i + 1] = new_indices[n + ni + i1];
            dst[i + 2] = new_indices[n + ni + i2];
            i += 3;
        }
        n += count;
    }
}

void MeshRefiner::genSubmeshes(IArray<int> materialIDs)
{
    submeshes.clear();

    new_indices_submeshes.resize(new_indices_triangulated.size());
    const int *indices_read = new_indices_triangulated.data();
    int *indices_write = new_indices_submeshes.data();

    int num_splits = (int)splits.size();
    int offset_faces = 0;
    RawVector<Submesh> tmp_submeshes;

    for (int spi = 0; spi < num_splits; ++spi) {
        auto& split = splits[spi];
        int offset_vertices = split.offset_vertices;

        // count triangle indices
        for (int fi = 0; fi < split.num_faces; ++fi) {
            int mid = materialIDs[offset_faces + fi] + 1; // -1 == no material. adjust to zero based
            while (mid >= (int)tmp_submeshes.size()) {
                int id = (int)tmp_submeshes.size();
                tmp_submeshes.push_back({});
                tmp_submeshes.back().materialID = id - 1;
            }
            tmp_submeshes[mid].num_indices += (counts[fi] - 2) * 3;
        }

        for (int mi = 0; mi < (int)tmp_submeshes.size(); ++mi) {
            auto& sm = tmp_submeshes[mi];
            sm.indices_write = indices_write;
            sm.offset_indices = (int)std::distance(new_indices_submeshes.data(), indices_write);
            indices_write += sm.num_indices;
        }

        // copy indices
        for (int fi = 0; fi < split.num_faces; ++fi) {
            int mid = materialIDs[offset_faces + fi] + 1;
            int count = counts[offset_faces + fi];
            int nidx = (count - 2) * 3;
            for (int i = 0; i < nidx; ++i) {
                *(tmp_submeshes[mid].indices_write++) = *(indices_read++) - offset_vertices;
            }
        }

        for (int mi = 0; mi < (int)tmp_submeshes.size(); ++mi) {
            auto& sm = tmp_submeshes[mi];
            if (sm.num_indices > 0) {
                ++split.num_submeshes;
                submeshes.push_back(sm);
            }
        }

        offset_faces += split.num_faces;
        tmp_submeshes.clear();
    }

    int total_submeshes = 0;
    for (int spi = 0; spi < num_splits; ++spi) {
        auto& split = splits[spi];
        split.offset_submeshes = total_submeshes;
        total_submeshes += split.num_submeshes;
        for (int smi = 0; smi < split.num_submeshes; ++smi) {
            auto& sm = submeshes[smi + split.offset_submeshes];
            sm.split_index = spi;
            sm.submesh_index = smi;
        }
    }
}

void MeshRefiner::clear()
{
    split_unit = 0;
    counts.reset();
    indices.reset();
    points.reset();
    for (auto& attr : attributes) {
        attr->clear();
        // attributes are placement new-ed, so need to call destructor manually
        attr->~IAttribute();
    }
    attributes.clear();

    old2new_indices.clear();
    new2old_points.clear();

    new_indices.clear();
    new_indices_triangulated.clear();
    new_indices_submeshes.clear();

    new_points.clear();
    splits.clear();
    submeshes.clear();
    num_new_indices = 0;
}

void MeshRefiner::refine()
{
    if (connection.v2f_counts.size() != points.size()) {
        connection.buildConnection(indices, counts, points);
    }

    int num_indices = (int)indices.size();
    new_points.reserve(num_indices);
    new_indices.reserve(num_indices);
    for (auto& attr : attributes) { attr->prepare((int)points.size(), (int)indices.size()); }

    old2new_indices.resize(num_indices, -1);

    int num_faces_total = (int)counts.size();
    int offset_faces = 0;
    int offset_indices = 0;
    int offset_vertices = 0;
    int num_faces = 0;
    int num_indices_triangulated = 0;

    auto add_new_split = [&]() {
        auto split = Split{};
        split.offset_faces = offset_faces;
        split.offset_indices = offset_indices;
        split.offset_vertices = offset_vertices;
        split.num_faces = num_faces;
        split.num_indices_triangulated = num_indices_triangulated;
        split.num_vertices = (int)new_points.size() - offset_vertices;
        split.num_indices = (int)new_indices.size() - offset_indices;
        splits.push_back(split);

        offset_faces += split.num_faces;
        offset_indices += split.num_indices;
        offset_vertices += split.num_vertices;

        num_new_indices += num_indices_triangulated;
        num_faces = 0;
        num_indices_triangulated = 0;
    };

    auto compare_all_attributes = [&](int ni, int ii) -> bool {
        for (auto& attr : attributes)
            if (!attr->compare(ni, ii)) { return false; }
        return true;
    };

    auto find_or_emit_vertex = [&](int vi, int ii) -> int {
        int offset = connection.v2f_offsets[vi];
        int connection_count = connection.v2f_counts[vi];
        for (int ci = 0; ci < connection_count; ++ci) {
            int& ni = old2new_indices[connection.v2f_indices[offset + ci]];
            if (ni != -1 && compare_all_attributes(ni, ii)) {
                return ni;
            }
            else if (ni == -1) {
                ni = (int)new_points.size();
                new_points.push_back(points[vi]);
                new2old_points.push_back(vi);
                for (auto& attr : attributes) { attr->emit(ii); }
                return ni;
            }
        }
        return 0;
    };

    int offset = 0;
    for (int fi = 0; fi < num_faces_total; ++fi) {
        int count = counts[fi];

        if (split_unit > 0 && (int)new_points.size() - offset_vertices + count > split_unit) {
            add_new_split();

            // clear vertex cache
            memset(old2new_indices.data(), -1, old2new_indices.size() * sizeof(int));
        }

        for (int ci = 0; ci < count; ++ci) {
            int ii = offset + ci;
            int vi = indices[ii];
            new_indices.push_back(find_or_emit_vertex(vi, ii));
        }
        ++num_faces;
        num_indices_triangulated += (count - 2) * 3;
        offset += count;
    }
    add_new_split();
}
