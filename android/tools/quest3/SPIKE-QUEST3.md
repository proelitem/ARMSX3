# Spike Quest 3 : quel driver Vulkan, et peut-il servir OpenXR ?

Point 1 du plan « ARMSX3 sur Meta Quest 3, Vulkan, stéréoscopie ». Objectif : en une à deux
sessions sur le casque, répondre à trois questions qui conditionnent toute l'architecture
VR. Rien ici ne demande de modifier l'app, sauf le patch optionnel décrit en section 4.

## 1. Les trois questions

| # | Question | Où est la réponse |
|---|----------|-------------------|
| A | Un Turnip chargé par adrenotools tourne-t-il vraiment sous Horizon OS ? | ligne `Vulkan driver identity` d'ARMSX3.log (doit nommer turnip, pas Qualcomm) et `adb logcat \| grep hook_impl` |
| B | Le jeu rend-il plusieurs minutes sans `VK_ERROR_OUT_OF_DEVICE_MEMORY` ni device lost ? | ARMSX3.log, logcat `DEBUG`/`AndroidRuntime` |
| C | Le driver expose-t-il `VK_ANDROID_external_memory_android_hardware_buffer` ? | lignes `XR readiness` d'ARMSX3.log (nécessite le patch, section 4) |

Pourquoi C : le runtime OpenXR du Quest fournit les images de swapchain sous forme
d'AHardwareBuffer importés dans le VkDevice de l'app. Sans cette extension, un backend
OpenXR ne peut pas utiliser le driver, quelle que soit sa vitesse.

Contexte connu avant le test :

- Quest 3 et 3S : Snapdragon XR2 Gen 2, Adreno 740 à 640 MHz, 8 Go. Driver d'origine
  Qualcomm v837 (Vulkan 1.3.295, le pack `Qualcomm_837_adpkg.zip` de K11MCH1 en est extrait).
- Un projet comparable (WoWForeverywhere, oct. 2026) rapporte que tout Turnip custom échoue
  sur Quest 3 à l'init du device (`tu_device.cc`, BO init, `VK_ERROR_OUT_OF_DEVICE_MEMORY`)
  et a fini par forcer le driver d'origine sur ce casque. D'autres (WinlatorXR) annoncent
  Turnip fonctionnel. Le test tranche.
- StevenMXZ v33 est un build « Turnip Gen8 » pour Adreno 8xx : hors sujet pour un 740.
  « WinTurnip 1.09 » correspond aux WN-Turnip de WinNative-Emu ; la 1.19 est l'actuelle.

## 2. Matériel et préparation (une fois)

1. Casque en mode développeur (app Meta Horizon > Casque > Mode développeur), câble USB,
   accepter le débogage dans le casque. `adb devices` doit lister le Quest.
2. APK ARMSX3 1.5 : prendre `ARMSX3-1.5-a13-armv8.2-sdk33.apk` (Horizon OS est basé sur
   Android 14, API 34 ; la variante sdk35 ne s'installera pas). Repli : `legacy-armv8.1-sdk30`.
   Téléchargement : `https://github.com/ARMSX2/ARMSX3/releases/download/1.5/<nom>`.
3. Firmware PS3 et un jeu de test léger, déjà connus pour tourner sur Adreno 7xx sous ARMSX3.
   Même jeu pour tous les essais. Idéalement un titre PSN 2D ou un jeu avec option 3D native
   (ex. Super Stardust HD, Wipeout HD) pour préparer le point 4 du plan.
4. Manette : les Touch ne sont pas vues comme gamepad par une app 2D. Appairer une manette
   Bluetooth (Xbox/DualSense) au casque, ou utiliser les contrôles tactiles à la souris
   virtuelle, juste pour passer les menus.
5. Depuis un terminal (Git Bash sous Windows convient) :

```bash
cd android/tools/quest3
./quest3-driver-spike.sh info
./quest3-driver-spike.sh install /chemin/ARMSX3-1.5-a13-armv8.2-sdk33.apk
./quest3-driver-spike.sh push-drivers
```

`info` consigne le modèle, la version d'Horizon OS, le noyau, le GPU vu par KGSL et le driver
d'origine. Copiez sa sortie dans le compte rendu : c'est la première fois qu'on documente
cette combinaison pour ARMSX3.

## 3. Matrice d'essais

Un essai = un driver, le même jeu, 2 à 3 minutes de jeu, puis Entrée dans le script.

| Label | Driver | Comment l'activer dans ARMSX3 |
|-------|--------|-------------------------------|
| `stock` | Qualcomm d'origine | Backend > Driver GPU > « Système » |
| `k11-r7` | K11MCH1 Turnip v26.0.0 R7 (`Turnip_v26.0.0_R7.zip`) | Importer depuis `/sdcard/Download/armsx3-drivers/` |
| `wn-1.19` | WN-Turnip 1.19 balanced (`WN-Turnip-1.19-b_Axxx.zip`) | idem |
| `banners-1030` | Banners-Turnip A740 standard (`Turnip-v27.0.0-20261030.zip`) | idem |

Après chaque changement de driver, utiliser « Appliquer et redémarrer » puis relancer l'app.
Puis :

```bash
./quest3-driver-spike.sh run stock
./quest3-driver-spike.sh run k11-r7
./quest3-driver-spike.sh run wn-1.19
./quest3-driver-spike.sh run banners-1030
./quest3-driver-spike.sh summary
```

Chaque essai laisse dans `results/<label>/` : `logcat-full.txt`, `logcat-filtered.txt`,
`ARMSX3.log`, `app-logs/` (session.log et éventuels crash-*.txt) et `summary.txt` à compléter
avec le FPS lu sur l'overlay.

Si un Turnip ne charge pas, `summary.txt` montre la ligne `hook_impl` qui dit pourquoi
(souvent : symbole libc trop récent, ou dlopen refusé par SELinux Horizon). Noter aussi si
la variante Gmem/Sysmem de K11MCH1 change quelque chose uniquement si R7 standard échoue à
l'init du device, pas avant.

Essai bonus si le temps le permet : `stock` avec l'OpenGL/ANGLE de la 1.5, pour mesurer
l'écart et avoir un plan C.

## 4. Patch « XR readiness » (recommandé avant les essais)

`rpcs3/Emu/RSX/VK/vkutils/device.cpp` logue désormais, à la création du device et sur
Android seulement, la présence des extensions dont un backend OpenXR aura besoin, et conclut
par une ligne `AHardwareBuffer swapchain import is AVAILABLE / NOT AVAILABLE`. Le core doit
être recompilé (voir README, cible `rpcsx-android`) et la `libarmsx3-core.so` recopiée dans
l'app avant de construire l'APK. Sans ce patch, les essais répondent à A et B mais pas à C.

Si la recompilation n'est pas possible tout de suite : faire les essais A et B avec l'APK
1.5 officiel, et refaire un seul passage `stock` + le meilleur Turnip une fois le core
patché. C se lit alors en quelques secondes, dès l'écran de chargement.

## 5. Lecture des résultats et décision

| Résultat | Conséquence sur le plan |
|----------|-------------------------|
| Aucun Turnip ne charge, ou tous perdent le device | Quest 3 = driver Qualcomm d'origine. Garder les contournements Adreno existants (conditional rendering coupé, fp16). Le backend OpenXR se fait sur le driver stock, qui expose l'AHB par construction. |
| Un Turnip tourne et `AHB oui` | Meilleur cas : Turnip + OpenXR. Ce pack devient la recommandation du profil Quest dans `GpuInfo.kt`. |
| Un Turnip tourne mais `AHB NON` | Turnip utilisable seulement en mode panneau 2D. Pour la VR, driver stock. Prévoir le choix automatique de driver selon le mode d'affichage. |
| Stock plante aussi (mémoire) | Problème de budget mémoire Horizon, pas de driver : baisser VRAM/résolution dans le profil Quest avant toute autre chose. |

Dans tous les cas, noter le FPS : à 640 MHz, seuls les titres légers seront jouables, et
les jeux 3D natifs rendent deux fois la scène côté PS3.

## 6. Références

- Issue Quest 3 / Turnip (WoWForeverywhere) : https://github.com/YoruDev-Ryland/WoWForeverywhere/issues/2
- K11MCH1 AdrenoToolsDrivers (R7, Qualcomm v837 extrait du Quest 3) : https://github.com/K11MCH1/AdrenoToolsDrivers/releases
- WinNative-Emu WN-Turnip : https://github.com/WinNative-Emu/Drivers/releases
- Banners-Turnip A740 : https://github.com/niilo/Banners-Turnip-A740/releases
- XR_KHR_vulkan_enable2 : https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html
