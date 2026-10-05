// atom-modem.ino - the Atom modem, 2.0. Build it with PlatformIO:
//   pio run -e atoms3-lite      (M5Stack AtomS3 Lite, ESP32-S3)
//   pio run -e m5stack-atom     (the plain Atom Lite/Matrix, ESP32)
// The Arduino IDE no longer builds it on its own: the picture converter
// compiles PsiMail's decoders from ../../mail/engine/img and zlib's inflate
// from ../../ssh/zlib, which the IDE will not find from a sketch folder.
// All the code is in src/ (see src/main.cpp); this file is only here so the
// folder still opens as a sketch. See docs/UPGRADE.md.
