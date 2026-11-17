plugins { id("com.android.application") }

android {
    namespace = "com.xnuron.app"
    compileSdk = 34
    defaultConfig {
        applicationId = "com.xnuron.app"
        minSdk = 24
        targetSdk = 34
        versionCode = 437
        versionName = "0.4.387"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
