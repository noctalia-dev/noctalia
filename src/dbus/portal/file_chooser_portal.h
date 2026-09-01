#pragma once

#include <memory>

class SessionBus;

/// Backend for org.freedesktop.impl.portal.FileChooser.
///
/// Lets every application on the session get Noctalia's own file dialog instead
/// of the GTK one: xdg-desktop-portal routes FileChooser calls to whichever
/// backend declares the interface in a .portal file, so this is what makes the
/// shell's picker the system picker.
///
/// Opt-in. Registering the FileChooser role unconditionally would replace the
/// file dialog of every app the moment Noctalia updates, so the service is only
/// constructed when shell.portal_file_chooser is true.
///
/// Construction registers the bus name and object; throws on failure so the
/// caller can log and continue without the portal.
class FileChooserPortal {
public:
  explicit FileChooserPortal(SessionBus& bus);
  ~FileChooserPortal();

  FileChooserPortal(const FileChooserPortal&) = delete;
  FileChooserPortal& operator=(const FileChooserPortal&) = delete;
  FileChooserPortal(FileChooserPortal&&) = delete;
  FileChooserPortal& operator=(FileChooserPortal&&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
