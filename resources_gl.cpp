/*
 * Copyright (c) 2014-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2022 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

/* Contact ckubisch@nvidia.com (Christoph Kubisch) for feedback */


#include "resources_gl.hpp"
#include "renderer.hpp"

#include "nvmeshlet_builder.hpp"
#include <imgui/backends/imgui_impl_gl.h>

namespace meshlettest {

bool ResourcesGL::initFramebuffer(int width, int height, int supersample, bool vsync)
{
  width *= supersample;
  height *= supersample;

  m_framebuffer.renderWidth  = width;
  m_framebuffer.renderHeight = height;
  m_framebuffer.supersample  = supersample;

  nvgl::newTexture(m_framebuffer.texSceneColor, GL_TEXTURE_2D);
  nvgl::newTexture(m_framebuffer.texSceneDepthStencil, GL_TEXTURE_2D);
  nvgl::newFramebuffer(m_framebuffer.fboScene);

  glTextureStorage2D(m_framebuffer.texSceneColor, 1, GL_RGBA8, width, height);
  glTextureStorage2D(m_framebuffer.texSceneDepthStencil, 1, GL_DEPTH24_STENCIL8, width, height);

  glNamedFramebufferTexture(m_framebuffer.fboScene, GL_COLOR_ATTACHMENT0, m_framebuffer.texSceneColor, 0);
  glNamedFramebufferTexture(m_framebuffer.fboScene, GL_DEPTH_STENCIL_ATTACHMENT, m_framebuffer.texSceneDepthStencil, 0);

  return true;
}

void ResourcesGL::deinitFramebuffer()
{
  nvgl::deleteFramebuffer(m_framebuffer.fboScene);

  nvgl::deleteTexture(m_framebuffer.texSceneColor);
  nvgl::deleteTexture(m_framebuffer.texSceneDepthStencil);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ResourcesGL::deinit()
{
  deinitScene();
  deinitFramebuffer();
  deinitPrograms();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  m_profilerGL.deinit();

  glDeleteVertexArrays(1, &m_common.standardVao);

  m_common.viewBuffer.destroy();
  m_common.statsBuffer.destroy();
  m_common.statsReadBuffer.destroy();

  ImGui::ShutdownGL();
}

void ResourcesGL::deinitScene()
{
  m_scene.deinit();

  glFinish();
}

bool ResourcesGL::initScene(const CadScene& cadscene)
{
  m_fp16                = cadscene.m_cfg.fp16;
  m_extraAttributes     = cadscene.m_cfg.extraAttributes;
  m_vertexSize          = (uint32_t)cadscene.getVertexSize();
  m_vertexAttributeSize = (uint32_t)cadscene.getVertexAttributeSize();

  m_scene.init(cadscene);

  assert(sizeof(CadScene::MatrixNode) == m_alignedMatrixSize);
  assert(sizeof(CadScene::Material) == m_alignedMaterialSize);

  std::vector<CadSceneGL::GeometryUbo> geometryData(m_scene.m_geometryMem.getChunkCount());
  for(size_t i = 0; i < m_scene.m_geometryMem.getChunkCount(); i++)
  {
    uint64_t* geombindings = (uint64_t*)&geometryData[i];

    const GeometryMemoryGL::Chunk& chunk = m_scene.m_geometryMem.getChunk(i);

    geombindings[GEOMETRY_SSBO_MESHLETDESC] = (chunk.meshADDR);
    geombindings[GEOMETRY_SSBO_PRIM]        = (chunk.meshIndicesADDR);
    geombindings[GEOMETRY_TEX_VBO]          = (chunk.vboTEXADDR);
    geombindings[GEOMETRY_TEX_ABO]          = (chunk.aboTEXADDR);
  }

  m_setup.geometryBindings.create(sizeof(CadSceneGL::GeometryUbo) * geometryData.size(), geometryData.data(), 0, 0);

  return true;
}

bool ResourcesGL::initPrograms(const std::string& path, const std::string& prepend)
{
  m_progManager.addDirectory(path);
  m_progManager.addDirectory(std::string("GLSL_" PROJECT_NAME));
  m_progManager.addDirectory(path + std::string(PROJECT_RELDIRECTORY));

  m_progManager.m_prepend        = prepend + "#define IS_VULKAN 0\n";
  m_progManager.m_preprocessOnly = false;

  m_programids.draw_object_tris =
      m_progManager.createProgram(nvgl::ProgramManager::Definition(GL_VERTEX_SHADER, "draw.vert.glsl"),
                                  nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "draw.frag.glsl"));


  m_programids.draw_bboxes =
      m_progManager.createProgram(nvgl::ProgramManager::Definition(GL_VERTEX_SHADER, "meshletbbox.vert.glsl"),
                                  nvgl::ProgramManager::Definition(GL_GEOMETRY_SHADER, "meshletbbox.geo.glsl"),
                                  nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "meshletbbox.frag.glsl"));

  if(m_nativeMeshSupport)
  {
    m_programids.draw_object_mesh = m_progManager.createProgram(
        nvgl::ProgramManager::Definition(GL_MESH_SHADER_NV, "#define USE_TASK_STAGE 0\n", "drawmeshlet_nv_basic.mesh.glsl"),
        nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "drawmeshlet_nv.frag.glsl"));
    m_programids.draw_object_mesh_task = m_progManager.createProgram(
        nvgl::ProgramManager::Definition(GL_TASK_SHADER_NV, "drawmeshlet_nv.task.glsl"),
        nvgl::ProgramManager::Definition(GL_MESH_SHADER_NV, "#define USE_TASK_STAGE 1\n", "drawmeshlet_nv_basic.mesh.glsl"),
        nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "drawmeshlet_nv.frag.glsl"));

    m_programids.draw_object_cull_mesh = m_progManager.createProgram(
        nvgl::ProgramManager::Definition(GL_MESH_SHADER_NV, "#define USE_TASK_STAGE 0\n", "drawmeshlet_nv_cull.mesh.glsl"),
        nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "drawmeshlet_nv.frag.glsl"));
    m_programids.draw_object_cull_mesh_task = m_progManager.createProgram(
        nvgl::ProgramManager::Definition(GL_TASK_SHADER_NV, "drawmeshlet_nv.task.glsl"),
        nvgl::ProgramManager::Definition(GL_MESH_SHADER_NV, "#define USE_TASK_STAGE 1\n", "drawmeshlet_nv_cull.mesh.glsl"),
        nvgl::ProgramManager::Definition(GL_FRAGMENT_SHADER, "drawmeshlet_nv.frag.glsl"));
  }

  updatedPrograms();

  return m_progManager.areProgramsValid();
}

void ResourcesGL::reloadPrograms(const std::string& prepend)
{
  m_progManager.m_prepend = prepend;
  m_progManager.reloadPrograms();
  updatedPrograms();
}

void ResourcesGL::updatedPrograms()
{
  m_programs.draw_object_tris = m_progManager.get(m_programids.draw_object_tris);
  m_programs.draw_bboxes      = m_progManager.get(m_programids.draw_bboxes);
  if(m_nativeMeshSupport)
  {
    m_programs.draw_object_mesh      = m_progManager.get(m_programids.draw_object_mesh);
    m_programs.draw_object_mesh_task = m_progManager.get(m_programids.draw_object_mesh_task);

    m_programs.draw_object_cull_mesh      = m_progManager.get(m_programids.draw_object_cull_mesh);
    m_programs.draw_object_cull_mesh_task = m_progManager.get(m_programids.draw_object_cull_mesh_task);
  }
}

void ResourcesGL::deinitPrograms()
{
  m_progManager.destroyProgram(m_programids.draw_object_tris);
  if(m_nativeMeshSupport)
  {
    m_progManager.destroyProgram(m_programids.draw_object_mesh);
    m_progManager.destroyProgram(m_programids.draw_object_mesh_task);
    m_progManager.destroyProgram(m_programids.draw_object_cull_mesh);
    m_progManager.destroyProgram(m_programids.draw_object_cull_mesh_task);
  }

  glUseProgram(0);
}

bool ResourcesGL::init(const nvgl::ContextWindow* contextWindowGL, nvh::Profiler* profiler)
{
  const GLubyte* renderer = glGetString(GL_RENDERER);
  LOGI("GL device: %s\n", renderer);

  glGenVertexArrays(1, &m_common.standardVao);
  glBindVertexArray(m_common.standardVao);

  ImGui::InitGL();

  GLint uboAlignment;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uboAlignment);
  initAlignedSizes(uboAlignment);

  m_profilerGL = nvgl::ProfilerGL(profiler);
  m_profilerGL.init();
  m_nativeMeshSupport = has_GL_NV_mesh_shader != 0;

  // Common
  m_common.viewBuffer.create(sizeof(SceneData), nullptr, GL_DYNAMIC_STORAGE_BIT, 0);
  m_common.statsBuffer.create(sizeof(CullStats), nullptr, GL_DYNAMIC_STORAGE_BIT, 0);
  m_common.statsReadBuffer.create(sizeof(CullStats) * CYCLED_FRAMES, nullptr,
                                  GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_CLIENT_STORAGE_BIT, 0);

  return true;
}


void ResourcesGL::blitFrame(const FrameConfig& global)
{
  const nvgl::ProfilerGL::Section profile(m_profilerGL, "BltUI");

  // blit to background
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffer.fboScene);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, m_framebuffer.renderWidth, m_framebuffer.renderHeight, 0, 0, global.winWidth,
                    global.winHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if(global.imguiDrawData)
  {
    glViewport(0, 0, global.winWidth, global.winHeight);
    ImGui::RenderDrawDataGL(global.imguiDrawData);
  }
}

nvmath::mat4f ResourcesGL::perspectiveProjection(float fovy, float aspect, float nearPlane, float farPlane) const
{
  return nvmath::perspective(fovy, aspect, nearPlane, farPlane);
}

void ResourcesGL::getStats(CullStats& stats)
{
  stats = ((const CullStats*)m_common.statsReadBuffer.mapped)[m_frame % CYCLED_FRAMES];
}

void ResourcesGL::copyStats() const
{
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
  glCopyNamedBufferSubData(m_common.statsBuffer, m_common.statsReadBuffer, 0,
                           sizeof(CullStats) * (m_frame % CYCLED_FRAMES), sizeof(CullStats));
}

void ResourcesGL::drawBoundingBoxes(const class RenderList* NV_RESTRICT list) const
{
  size_t vertexSize = list->m_scene->getVertexSize();

  glUseProgram(m_programs.draw_bboxes);
  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_SCENE_VIEW, m_common.viewBuffer.buffer);
  glDisable(GL_CULL_FACE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glLineWidth(m_framebuffer.supersample);
  {
    int  lastMaterial = -1;
    int  lastGeometry = -1;
    int  lastMatrix   = -1;
    int  lastChunk    = -1;

    for(int i = 0; i < list->m_drawItems.size(); i++)
    {
      const RenderList::DrawItem& di = list->m_drawItems[i];

      if(lastGeometry != di.geometryIndex)
      {
        const CadSceneGL::Geometry& geogl = m_scene.m_geometry[di.geometryIndex];
        int                         chunk = int(geogl.mem.chunkIndex);

        if(chunk != lastChunk)
        {
          glBindBufferRange(GL_UNIFORM_BUFFER, UBO_GEOMETRY, m_setup.geometryBindings.buffer,
                            sizeof(CadSceneGL::GeometryUbo) * chunk, sizeof(CadSceneGL::GeometryUbo));

          lastChunk  = chunk;
        }

        glUniform4ui(0, uint32_t(geogl.topoMeshlet.offset / sizeof(NVMeshlet::MeshletDesc)), 0, 0, 0);

        lastGeometry = di.geometryIndex;
      }

      if(lastMatrix != di.matrixIndex)
      {
        glBindBufferRange(GL_UNIFORM_BUFFER, UBO_OBJECT, m_scene.m_buffers.matrices.buffer,
                          m_alignedMatrixSize * di.matrixIndex, sizeof(CadScene::MatrixNode));

        lastMatrix = di.matrixIndex;
      }

      glDrawArrays(GL_POINTS, di.meshlet.offset, di.meshlet.count);
    }
  }

  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_SCENE_VIEW, 0);
  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_OBJECT, 0);
  glBindBufferBase(GL_UNIFORM_BUFFER, UBO_GEOMETRY, 0);

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void ResourcesGL::enableVertexFormat() const
{
  glVertexAttribBinding(VERTEX_POS, 0);
  glVertexAttribBinding(VERTEX_NORMAL, 1);
  glEnableVertexAttribArray(VERTEX_POS);
  glEnableVertexAttribArray(VERTEX_NORMAL);
  for(uint32_t i = 0; i < m_extraAttributes; i++)
  {
    glEnableVertexAttribArray(VERTEX_EXTRAS + i);
    glVertexAttribBinding(VERTEX_EXTRAS + i, 1);
  }

  if(m_fp16)
  {
    glVertexAttribFormat(VERTEX_POS, 3, GL_HALF_FLOAT, GL_FALSE, offsetof(CadScene::VertexFP16, position));
    glVertexAttribFormat(VERTEX_NORMAL, 3, GL_HALF_FLOAT, GL_FALSE, offsetof(CadScene::VertexAttributesFP16, normal));
    for(uint32_t i = 0; i < m_extraAttributes; i++)
    {
      glVertexAttribFormat(VERTEX_EXTRAS + i, 4, GL_HALF_FLOAT, GL_FALSE,
                           sizeof(CadScene::VertexAttributesFP16) + sizeof(half) * 4 * i);
    }
  }
  else
  {
    glVertexAttribFormat(VERTEX_POS, 3, GL_FLOAT, GL_FALSE, offsetof(CadScene::Vertex, position));
    glVertexAttribFormat(VERTEX_NORMAL, 3, GL_FLOAT, GL_FALSE, offsetof(CadScene::VertexAttributes, normal));
    for(uint32_t i = 0; i < m_extraAttributes; i++)
    {
      glVertexAttribFormat(VERTEX_EXTRAS + i, 4, GL_FLOAT, GL_FALSE, sizeof(CadScene::VertexAttributes) + sizeof(float) * 4 * i);
    }
  }
  glBindVertexBuffer(0, 0, 0, m_vertexSize);
  glBindVertexBuffer(1, 0, 0, m_vertexAttributeSize);
}


void ResourcesGL::disableVertexFormat() const
{
  glDisableVertexAttribArray(VERTEX_POS);
  glDisableVertexAttribArray(VERTEX_NORMAL);
  for(uint32_t i = 0; i < m_extraAttributes; i++)
  {
    glVertexAttribBinding(VERTEX_EXTRAS + i, i);
    glDisableVertexAttribArray(VERTEX_EXTRAS + i);
  }
  glBindVertexBuffer(0, 0, 0, 16);
  glBindVertexBuffer(1, 0, 0, 16);
}
}  // namespace meshlettest
