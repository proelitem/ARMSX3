# Compiler ARMSX3 pour la Quest 3 (branche `quest3/touch-input`)

Ce guide est écrit pour être exécuté par un agent (Claude Code, Cowork) sur un PC
Windows, ou à la main. Les scripts du dépôt (`android/configure.sh`,
`android/build-variants.sh`) ont été écrits pour macOS : `build-variants.sh` utilise le
`llvm-strip` du dossier `darwin-x86_64` et ne fonctionne pas tel quel sous Windows.
Les étapes ci-dessous font la même chose à la main. Sous WSL ou Linux, les scripts
fonctionnent presque sans changement, voir la fin.

Budget : la première compilation construit LLVM 22 en statique. Comptez 2 à 4 heures,
environ 40 Go de disque et 16 Go de RAM minimum (32 recommandés pour l'édition de
liens). Les compilations suivantes ne refont que ce qui a changé.

## 1. Outils

- Android Studio, puis dans le SDK Manager (onglet SDK Tools, cocher « Show Package
  Details ») : **NDK 29.0.14206865** (pas 28 : clang 19 ne compile pas cet arbre),
  **CMake 3.30.5**, Platform-Tools (adb), Android SDK Platform 35.
- Git pour Windows (fournit Git Bash, nécessaire pour `configure.sh`).
- Un JDK 21 sur le PATH ou dans `JAVA_HOME` ; Gradle le téléchargera sinon
  (`gradle-daemon-jvm.properties`).
- Le casque en mode développeur, relié en USB, débogage autorisé.

Dans Git Bash :

```bash
export ANDROID_HOME="$LOCALAPPDATA/Android/Sdk"
export NDK="$ANDROID_HOME/ndk/29.0.14206865"
export PATH="$ANDROID_HOME/cmake/3.30.5/bin:$ANDROID_HOME/platform-tools:$PATH"
```

## 2. Sources

```bash
git clone --recursive -b quest3/touch-input https://github.com/proelitem/armsx3.git
cd armsx3
git clone https://github.com/SnowflakePowered/librashader 3rdparty/librashader
git clone https://github.com/bylaws/libadrenotools android/armsx3-ui/app/src/main/cpp/libadrenotools
```

La branche `quest3/touch-input` contient les cinq étapes Quest (spike drivers, profil
casque, présentation OpenXR, stéréo par œil, manettes Touch). Le sous-module LLVM est
gros (plusieurs Go) ; `--recursive` est obligatoire.

## 3. Le core (libarmsx3-core.so)

Configuration, équivalente à la variante `a13` de `build-variants.sh` (API 33,
armv8.2 avec dotprod et fp16, celle que la Quest 3 installe) :

```bash
bash android/stamp-git-version.sh
NDK_VERSION=29.0.14206865 ANDROID_API=33 BUILD_DIR="$PWD/build-a13" \
  bash android/configure.sh "-DARMSX3_ARM_MARCH=armv8.2-a+dotprod+fp16"
```

Vérifier que la sortie de cmake contient `ARMSX3: OpenXR presentation ON`. Puis :

```bash
ninja -C build-a13 android/libarmsx3-core.so
```

La cible `armsx3_lsfg` (génération d'images) est facultative et séparée ; elle n'est
pas nécessaire pour les essais Quest.

Strip et mise en place (chemin `windows-x86_64` du NDK ; `linux-x86_64` sous WSL) :

```bash
STRIP="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
mkdir -p android/armsx3-ui/app/src/main/jniLibs/arm64-v8a
"$STRIP" --strip-unneeded -o android/armsx3-ui/app/src/main/jniLibs/arm64-v8a/libarmsx3-core.so \
  build-a13/android/libarmsx3-core.so
```

## 4. L'application (APK)

```bash
cd android/armsx3-ui
./gradlew.bat :app:assembleGithubRelease -Parmsx3.minSdk=33
```

L'APK est dans `app/build/outputs/apk/github/release/app-github-release.apk`, signé
avec la clé de débogage (suffisant pour un sideload). La première exécution de Gradle
télécharge les dépendances, dont le loader OpenXR
(`org.khronos.openxr:openxr_loader_for_android`).

Si Gradle se plaint de la version de Java, installer un JDK 21 et le mettre dans
`JAVA_HOME`.

## 5. Installation et premier essai

```bash
adb install -r -g app/build/outputs/apk/github/release/app-github-release.apk
```

Dans le casque : Bibliothèque > Sources inconnues > ARMSX3. Installer le firmware PS3
et un jeu depuis le panneau 2D comme sur un téléphone. Au lancement d'un jeu, l'app
bascule dans l'activité immersive : écran virtuel, manettes Touch actives.

Pour revenir au panneau 2D : Paramètres > Rendu > désactiver « Écran virtuel (OpenXR) ».

Logs à récupérer après un essai (ARMSX3.log contient les canaux `XR` et
« XR readiness ») :

```bash
adb logcat -d | grep -E "XR|RPCSX-UI|hook_impl|AndroidRuntime|DEBUG" > logcat.txt
adb pull /sdcard/Android/data/com.armsx3/files/cache/ARMSX3.log
```

Le script `quest3-driver-spike.sh` de ce dossier automatise cette collecte et la
comparaison des drivers (voir SPIKE-QUEST3.md).

## 6. Sous WSL ou Linux

Les scripts du dépôt fonctionnent avec un SDK Android installé côté Linux :

```bash
export ANDROID_HOME=$HOME/Android/Sdk
sed -i 's/darwin-x86_64/linux-x86_64/' android/build-variants.sh
android/build-variants.sh a13
```

L'APK est alors copié dans `~/Downloads/ARMSX3-1.5-a13-armv8.2-sdk33.apk`. adb doit
voir le casque depuis WSL (usbipd) ou l'APK est installé depuis Windows.

## 7. Prompt prêt à coller pour l'agent local

> Compile ARMSX3 pour ma Meta Quest 3 en suivant `android/tools/quest3/BUILD-QUEST3.md`
> de la branche `quest3/touch-input` du dépôt https://github.com/proelitem/armsx3.
> Installe les outils manquants (Android Studio SDK avec NDK 29.0.14206865 et CMake
> 3.30.5, JDK 21, Git), clone les sources avec les sous-modules et les deux dépôts
> annexes, construis le core avec la configuration a13 (API 33, armv8.2+dotprod+fp16),
> vérifie que cmake affiche « ARMSX3: OpenXR presentation ON », strip et copie la
> bibliothèque, construis l'APK `assembleGithubRelease` avec `-Parmsx3.minSdk=33`, puis
> installe-le sur le casque branché en USB avec `adb install -r -g`. Si une étape échoue,
> montre-moi l'erreur complète avant de tenter un contournement. À la fin, donne-moi le
> chemin de l'APK et la sortie de `adb devices`.
