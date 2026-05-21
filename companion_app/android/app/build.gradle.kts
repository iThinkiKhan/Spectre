plugins {
  id("com.android.application")
  id("org.jetbrains.kotlin.android")
  id("com.facebook.react")
}

react {
  autolinkLibrariesWithApp()
}

val hermesEnabled = (project.findProperty("hermesEnabled") ?: "true").toString().toBoolean()

android {
  namespace = "com.spectre.companion"
  compileSdk = 36
  ndkVersion = "27.1.12297006"

  defaultConfig {
    applicationId = "com.spectre.companion"
    minSdk = 24
    targetSdk = 36
    versionCode = 1
    versionName = "0.1.0"
  }

  buildTypes {
  getByName("debug") {
    applicationIdSuffix = ".debug"
    isDebuggable = true
  }

  getByName("release") {
    signingConfig = signingConfigs.getByName("debug")
    isMinifyEnabled = false
    isShrinkResources = false
  }
}

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
  }

  kotlinOptions {
    jvmTarget = "17"
  }
}

dependencies {
  implementation("com.facebook.react:react-android")
  implementation(project(":react-native-ble-plx"))
  if (hermesEnabled) {
    implementation("com.facebook.react:hermes-android")
  } else {
    implementation("org.webkit:android-jsc:+")
  }
}
