plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// Release signing is pulled from the environment (KAGAMI_* — set in your shell
// or as CI secrets). When the vars are absent (e.g. a contributor without the
// key, or a fork PR), the release build stays unsigned instead of failing.
val kagamiKeystore: String? = System.getenv("KAGAMI_KEYSTORE")
val kagamiKeystorePassword: String? = System.getenv("KAGAMI_KEYSTORE_PASSWORD")
val kagamiKeyAlias: String? = System.getenv("KAGAMI_KEY_ALIAS")
val kagamiKeyPassword: String? = System.getenv("KAGAMI_KEY_PASSWORD")
val kagamiHasSigning = listOf(
    kagamiKeystore, kagamiKeystorePassword, kagamiKeyAlias, kagamiKeyPassword,
).all { !it.isNullOrBlank() }

android {
    namespace = "com.anatdx.kagami"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.anatdx.kagami"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    signingConfigs {
        if (kagamiHasSigning) {
            create("release") {
                storeFile = file(kagamiKeystore!!)
                storePassword = kagamiKeystorePassword
                keyAlias = kagamiKeyAlias
                keyPassword = kagamiKeyPassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (kagamiHasSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    buildFeatures {
        aidl = true
        compose = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        compilerOptions {
            jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        }
    }

    packaging {
        resources {
            excludes += "META-INF/*.version"
            excludes += "DebugProbesKt.bin"
            excludes += "kotlin-tooling-metadata.json"
        }
    }

    lint {
        checkReleaseBuilds = false
    }
}

dependencies {
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.libsu.core)
    implementation(libs.libsu.service)

    debugImplementation(libs.androidx.compose.ui.tooling)
}
