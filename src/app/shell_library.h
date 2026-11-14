#pragma once

// Configures the shared UI library for the shell: application identity, the shell's translations
// for library text (key names, Select placeholder), and the codec pack as image decoder. Called
// first thing in main(), and before main() in tests that exercise shell code.
void configureSharedLibrary();
