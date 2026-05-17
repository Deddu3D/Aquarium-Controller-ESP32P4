pluginManagement {
    repositories {
        maven { url = uri("http://127.0.0.1:18080/"); isAllowInsecureProtocol = true }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven { url = uri("http://127.0.0.1:18080/"); isAllowInsecureProtocol = true }
        mavenCentral()
    }
}

rootProject.name = "AquariumController"
include(":app")
