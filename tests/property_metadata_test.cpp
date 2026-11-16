#include "filemanager/property_metadata.h"
#include "filemanager/property_operations.h"

#include <cassert>

int main() {
  assert(filemanager::propertyFilesystemTypeDescription("nfs4") == "NFSv4");
  assert(filemanager::propertyFilesystemTypeDescription("nfs", "rw,vers=4.2,addr=10.0.0.2") == "NFSv4");
  assert(filemanager::propertyFilesystemTypeDescription("nfs", "rw,nfsvers=4") == "NFSv4");
  assert(filemanager::propertyFilesystemTypeDescription("nfs", "rw,vers=3") == "NFS");
  assert(filemanager::propertyFilesystemTypeDescription("nfs", "rw,vers=4.foo") == "NFS");
  assert(filemanager::propertyFilesystemTypeDescription("nfs", "rw") == "NFS");
  assert(filemanager::propertyFilesystemTypeDescription("ext4") == "ext4");
  filemanager::PropertyResult success;
  success.succeeded = 1;
  assert(!success.needsFeedback());
  success.skipped = 1;
  assert(success.needsFeedback());
  success.skipped = 0;
  success.failed = 1;
  assert(success.needsFeedback());
  success.failed = 0;
  success.ownershipEffects = true;
  assert(success.needsFeedback());

  using Step = filemanager::PropertyApplyStep;
  assert(filemanager::propertyApplyStep("original", "draft", false) == Step::Rename);
  assert(filemanager::propertyApplyStep("original", "draft", true) == Step::Rename);
  assert(filemanager::propertyApplyStep("draft", "draft", true) == Step::Metadata);
  assert(filemanager::propertyApplyStep("draft", "draft", false) == Step::None);
  assert(filemanager::propertyApplyStep("original", "", false) == Step::Rename);

  GFileInfo* info = g_file_info_new();
  g_file_info_set_file_type(info, G_FILE_TYPE_REGULAR);
  for (const auto* mime : {"text/plain", "application/gzip"}) {
    g_file_info_set_content_type(info, mime);
    char* expected = g_content_type_get_description(mime);
    assert(filemanager::propertyTypeDescription(info) == expected);
    g_free(expected);
  }
  g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
  assert(filemanager::propertyTypeDescription(info) == "Folder");
  g_file_info_set_is_symlink(info, true);
  assert(filemanager::propertyTypeDescription(info) == "Symbolic link");
  g_object_unref(info);
  info = g_file_info_new();
  g_file_info_set_file_type(info, G_FILE_TYPE_REGULAR);
  assert(filemanager::propertyTypeDescription(info) == "File");
  g_object_unref(info);
  assert(filemanager::propertyFilenameSuffix("gamecontrollerdb.txt-backup", false) == ".txt-backup");
  assert(filemanager::propertyFilenameSuffix("folder.test", true).empty());
  assert(filemanager::propertyFilenameSuffix("README", false).empty());
  assert(filemanager::propertyMountTarget("/", "/"));
  assert(filemanager::propertyMountTarget("/mnt/Volume/", "/mnt/Volume"));
  assert(!filemanager::propertyMountTarget("/home/user/file", "/"));
  assert(!filemanager::propertyMountTarget("/mnt/Volume/child", "/mnt/Volume"));
  assert(!filemanager::propertyMountTarget("/mnt/Volume-other", "/mnt/Volume"));
}
