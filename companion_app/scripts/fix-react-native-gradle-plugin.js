const fs = require('fs');
const path = require('path');

const targetPath = path.join(
  __dirname,
  '..',
  'node_modules',
  '@react-native',
  'gradle-plugin',
  'settings.gradle.kts',
);

const oldSnippet =
  'plugins { id("org.gradle.toolchains.foojay-resolver-convention").version("0.5.0") }';
const newSnippet =
  'plugins { id("org.gradle.toolchains.foojay-resolver-convention").version("1.0.0") }';

if (!fs.existsSync(targetPath)) {
  process.exit(0);
}

const source = fs.readFileSync(targetPath, 'utf8');

if (!source.includes(oldSnippet)) {
  process.exit(0);
}

fs.writeFileSync(targetPath, source.replace(oldSnippet, newSnippet), 'utf8');
console.log('Patched React Native Gradle Foojay resolver to 1.0.0 for Gradle 9 compatibility.');
