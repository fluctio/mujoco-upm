// Copyright 2023 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include <mujoco/mjplugin.h>
#include <mujoco/mjtnum.h>
#include <mujoco/mujoco.h>
#include "elasticity.h"
#include "membrane.h"


namespace mujoco::plugin::elasticity {
namespace {

// local tetrahedron numbering
constexpr int kNumEdges = Stencil2D::kNumEdges;
constexpr int kNumVerts = Stencil2D::kNumVerts;

// area of a triangle
mjtNum ComputeVolume(const mjtNum* x, const int v[kNumVerts]) {
  mjtNum normal[3];
  mjtNum edge1[3];
  mjtNum edge2[3];

  mju_sub3(edge1, x+3*v[1], x+3*v[0]);
  mju_sub3(edge2, x+3*v[2], x+3*v[0]);
  mju_cross(normal, edge1, edge2);

  return mju_norm3(normal) / 2;
}

// compute local basis
void ComputeBasis(mjtNum basis[9], const mjtNum* x, const int v[kNumVerts],
                  const int faceL[2], const int faceR[2], mjtNum area) {
  mjtNum basisL[3], basisR[3];
  mjtNum edgesL[3], edgesR[3];
  mjtNum normal[3];

  mju_sub3(edgesL, x+3*v[faceL[0]], x+3*v[faceL[1]]);
  mju_sub3(edgesR, x+3*v[faceR[1]], x+3*v[faceR[0]]);

  mju_cross(normal, edgesR, edgesL);
  mju_normalize3(normal);
  mju_cross(basisL, normal, edgesL);
  mju_cross(basisR, edgesR, normal);

  // we use as basis the symmetrized tensor products of the edge normals of the
  // other two edges; this is shown in Weischedel "A discrete geometric view on
  // shear-deformable shell models" in the remark at the end of section 4.1;
  // equivalent to linear finite elements but in a coordinate-free formulation.

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      basis[3*i+j] = ( basisL[i]*basisR[j] +
                       basisR[i]*basisL[j] ) / (8*area*area);
    }
  }
}

}  // namespace

// factory function
std::optional<Membrane> Membrane::Create(const mjModel* m, mjData* d,
                                         int instance) {
  if (CheckAttr("face", m, instance) && CheckAttr("poisson", m, instance) &&
      CheckAttr("young", m, instance) && CheckAttr("thickness", m, instance)) {
    mjtNum nu = strtod(mj_getPluginConfig(m, instance, "poisson"), nullptr);
    mjtNum E = strtod(mj_getPluginConfig(m, instance, "young"), nullptr);
    mjtNum thick =
        strtod(mj_getPluginConfig(m, instance, "thickness"), nullptr);
    mjtNum damp =
            strtod(mj_getPluginConfig(m, instance, "damping"), nullptr);
    return Membrane(m, d, instance, nu, E, thick, damp);
  } else {
    mju_warning("Invalid parameter specification in shell plugin");
    return std::nullopt;
  }
}

// plugin constructor
Membrane::Membrane(const mjModel* m, mjData* d, int instance, mjtNum nu,
                   mjtNum E, mjtNum thick, mjtNum damp)
    : f0(-1), damping(damp), thickness(thick) {
  // count plugin bodies
  nv = ne = 0;
  for (int i = 1; i < m->nbody; i++) {
    if (m->body_plugin[i] == instance) {
      if (!nv++) {
        i0 = i;
      }
    }
  }

  // count flexes
  for (int i = 0; i < m->nflex; i++) {
    for (int j = 0; j < m->flex_vertnum[i]; j++) {
      if (m->flex_vertbodyid[m->flex_vertadr[i]+j] == i0) {
        f0 = i;
        nv = m->flex_vertnum[f0];
      }
    }
  }

  // vertex positions
  mjtNum* body_pos = m->flex_xvert0 + 3*m->flex_vertadr[f0];

  // loop over all triangles
  const int* elem = m->flex_elem + m->flex_elemdataadr[f0];
  for (int t = 0; t < m->flex_elemnum[f0]; t++) {
    const int* v = elem + (m->flex_dim[f0]+1) * t;
    for (int i = 0; i < kNumVerts; i++) {
      int bi = m->flex_vertbodyid[m->flex_vertadr[f0]+v[i]];
      if (bi && m->body_plugin[bi] != instance) {
        mju_error("Body %d does not have plugin instance %d", bi, instance);
      }
    }

    // triangles area
    mjtNum volume = ComputeVolume(body_pos, v);

    // material parameters
    mjtNum mu = E / (2*(1+nu)) * mju_abs(volume) / 4 * thickness;
    mjtNum la = E*nu / ((1+nu)*(1-2*nu)) * mju_abs(volume) / 4 * thickness;

    // local geometric quantities
    mjtNum basis[kNumEdges][9] = {{0}, {0}, {0}};

    // compute edge basis
    for (int e = 0; e < kNumEdges; e++) {
      ComputeBasis(basis[e], body_pos, v,
                   Stencil2D::edge[Stencil2D::edge[e][0]],
                   Stencil2D::edge[Stencil2D::edge[e][1]], volume);
    }

    // compute metric tensor
    // TODO: do not write in a const mjModel
    MetricTensor<Stencil2D>(m->flex_stiffness + 21 * m->flex_elemadr[f0], t, mu,
                            la, basis);
  }

  // allocate array
  ne = m->flex_edgenum[f0];
  elongation.assign(ne, 0);
  force.assign(3*nv, 0);
}

void Membrane::Compute(const mjModel* m, mjData* d, int instance) {
  mjtNum kD = damping / m->opt.timestep;

  // read edge lengths
  mjtNum* deformed = d->flexedge_length + m->flex_edgeadr[f0];
  mjtNum* ref = m->flexedge_length0 + m->flex_edgeadr[f0];

  // m->flexedge_length0 is not initialized when the plugin is constructed
  if (prev.empty()) {
    prev.assign(ne, 0);
    memcpy(prev.data(), ref, sizeof(mjtNum) * ne);
  }

  // we add generalized Rayleigh damping as decribed in Section 5.2 of
  // Kharevych et al., "Geometric, Variational Integrators for Computer
  // Animation" http://multires.caltech.edu/pubs/DiscreteLagrangian.pdf

  for (int idx = 0; idx < ne; idx++) {
    elongation[idx] = deformed[idx]*deformed[idx] - ref[idx]*ref[idx] +
                    ( deformed[idx]*deformed[idx] - prev[idx]*prev[idx] ) * kD;
  }

  // compute gradient of elastic energy and insert into passive force
  int flex_vertadr = m->flex_vertadr[f0];
  mjtNum* xpos = d->flexvert_xpos + 3*flex_vertadr;
  mjtNum* qfrc = d->qfrc_passive;

  ComputeForce<Stencil2D>(force, elongation, m, f0, xpos);

  // insert into passive force
  AddFlexForce(qfrc, force, m, d, xpos, f0);

  // update stored lengths
  if (kD > 0) {
    memcpy(prev.data(), deformed, sizeof(mjtNum) * ne);
  }
}



void Membrane::RegisterPlugin() {
  mjpPlugin plugin;
  mjp_defaultPlugin(&plugin);

  plugin.name = "mujoco.elasticity.membrane";
  plugin.capabilityflags |= mjPLUGIN_PASSIVE;

  const char* attributes[] = {"face", "edge", "young", "poisson", "thickness", "damping"};
  plugin.nattribute = sizeof(attributes) / sizeof(attributes[0]);
  plugin.attributes = attributes;
  plugin.nstate = +[](const mjModel* m, int instance) { return 0; };

  plugin.init = +[](const mjModel* m, mjData* d, int instance) {
    auto elasticity_or_null = Membrane::Create(m, d, instance);
    if (!elasticity_or_null.has_value()) {
      return -1;
    }
    d->plugin_data[instance] = reinterpret_cast<uintptr_t>(
        new Membrane(std::move(*elasticity_or_null)));
    return 0;
  };
  plugin.destroy = +[](mjData* d, int instance) {
    delete reinterpret_cast<Membrane*>(d->plugin_data[instance]);
    d->plugin_data[instance] = 0;
  };
  plugin.compute = +[](const mjModel* m, mjData* d, int instance, int type) {
    auto* elasticity = reinterpret_cast<Membrane*>(d->plugin_data[instance]);
    elasticity->Compute(m, d, instance);
  };

  mjp_registerPlugin(&plugin);
}

}  // namespace mujoco::plugin::elasticity
