import org.gradle.api.initialization.resolve.RepositoriesMode

pluginManagement {
  includeBuild("../node_modules/@react-native/gradle-plugin")
  repositories {
    google()
    mavenCentral()
    gradlePluginPortal()
  }
}

plugins {
  id("com.facebook.react.settings")
}

extensions.configure<com.facebook.react.ReactSettingsExtension> {
  autolinkLibrariesFromCommand()
}

rootProject.name = "companion_app"
include(":app")
include(":react-native-ble-plx")
project(":react-native-ble-plx").projectDir = file("../node_modules/react-native-ble-plx/android")

dependencyResolutionManagement {
  repositoriesMode.set(RepositoriesMode.PREFER_PROJECT)
  repositories {
    google()
    mavenCentral()
    maven { url = uri("../node_modules/react-native/android") }
    maven { url = uri("../node_modules/jsc-android/dist") }
  }
}
