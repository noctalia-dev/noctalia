#pragma once

#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>

class TextureId;

class ImageProgram {
public:
  ImageProgram() = default;
  ~ImageProgram() = default;

  ImageProgram(const ImageProgram&) = delete;
  ImageProgram& operator=(const ImageProgram&) = delete;

  void ensureInitialized();
  void destroy();

  void draw(
      TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, const Color& tint,
      bool monochromeTint, bool alphaMaskTint, float opacity, float radius = 0.0f,
      const Color& borderColor = {0.0f, 0.0f, 0.0f, 0.0f}, float borderWidth = 0.0f, int fitMode = 0,
      float textureWidth = 0.0f, float textureHeight = 0.0f, const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  GLint m_positionLocation = -1;
  GLint m_texCoordLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_rectLocation = -1;
  GLint m_tintLocation = -1;
  GLint m_opacityLocation = -1;
  GLint m_radiusLocation = -1;
  GLint m_borderColorLocation = -1;
  GLint m_borderWidthLocation = -1;
  GLint m_texSizeLocation = -1;
  GLint m_fitModeLocation = -1;
  GLint m_samplerLocation = -1;
  GLint m_transformLocation = -1;
  GLint m_monochromeLocation = -1;
  GLint m_alphaMaskLocation = -1;
};
