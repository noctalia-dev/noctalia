#pragma once

#include <GLES2/gl2.h>

class ShaderProgram {
public:
  ShaderProgram() = default;
  ~ShaderProgram();

  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;

  ShaderProgram(ShaderProgram&& other) noexcept;
  ShaderProgram& operator=(ShaderProgram&& other) noexcept;

  void create(const char* vertexSource, const char* fragmentSource);
  void destroy();
  // Forget the GL name without deleting it. Used after a robust-context reset, when the
  // program is already invalid and no context is current to delete it from.
  void abandon() noexcept;

  [[nodiscard]] bool isValid() const noexcept;
  [[nodiscard]] GLuint id() const noexcept;

private:
  GLuint m_program = 0;
};
