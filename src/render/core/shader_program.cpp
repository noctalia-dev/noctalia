#include "render/core/shader_program.h"

#include <stdexcept>
#include <string>

namespace {

  GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
      throw std::runtime_error("glCreateShader failed");
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
      GLint logLength = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
      std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
      glGetShaderInfoLog(shader, logLength, nullptr, log.data());
      glDeleteShader(shader);
      throw std::runtime_error("shader compilation failed: " + log);
    }

    return shader;
  }

} // namespace

ShaderProgram::~ShaderProgram() { destroy(); }

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept : m_program(other.m_program) { other.m_program = 0; }

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  destroy();
  m_program = other.m_program;
  other.m_program = 0;
  return *this;
}

void ShaderProgram::create(const char* vertexSource, const char* fragmentSource) {
  destroy();

  const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
  const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);

  m_program = glCreateProgram();
  if (m_program == 0) {
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    throw std::runtime_error("glCreateProgram failed");
  }

  glAttachShader(m_program, vertexShader);
  glAttachShader(m_program, fragmentShader);
  glLinkProgram(m_program);

  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint linked = 0;
  glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    GLint logLength = 0;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
    glGetProgramInfoLog(m_program, logLength, nullptr, log.data());
    destroy();
    throw std::runtime_error("shader link failed: " + log);
  }
}

void ShaderProgram::destroy() {
  if (m_program != 0) {
    glDeleteProgram(m_program);
    m_program = 0;
  }
}

void ShaderProgram::abandon() noexcept { m_program = 0; }

bool ShaderProgram::isValid() const noexcept { return m_program != 0; }

GLuint ShaderProgram::id() const noexcept { return m_program; }
