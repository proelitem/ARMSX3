plugins {
    id("com.android.application")
}

// memprobe: a throwaway measurement app for the Quest 3 memory question
// (android/tools/quest3/MEMOIRE-QUEST3.md). No dependencies on purpose: plain Views and one
// native file, so it builds with the SDK that builds ARMSX3 and nothing else.
android {
    namespace = "com.armsx3.memprobe"
    compileSdk = 37
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.armsx3.memprobe"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=c++_static") }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.5"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
