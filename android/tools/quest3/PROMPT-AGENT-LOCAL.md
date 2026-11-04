# Prompt pour l'agent local (reprise du projet Quest 3)

À coller tel quel à l'agent qui a accès au PC Windows et aux casques.

---

Tu reprends deux dépôts GitHub préparés par un agent distant qui n'a pas pu compiler ni tester sur le matériel. Ton travail : compiler, corriger ce qui ne compile pas, vérifier sur deux Meta Quest 3, et rapporter point par point. Ne réécris pas l'architecture ; corrige au plus près, un commit par correction, message clair en anglais.

## Dépôts

- **ARMSX3** (émulateur PS3, fork de ARMSX2/ARMSX3) : `https://github.com/proelitem/armsx3`, branche `master`, commit `fc79582` ou plus récent. Dépôt amont `https://github.com/ARMSX2/ARMSX3` en remote `upstream`.
- **Tremu** (lanceur VR, ex-WFGEmu) : `https://github.com/proelitem/tremu`, branche `main`, commit `79b0b77`. Mon clone local est dans `G:\Projets\WFVR\wgemu` avec du travail non commité (tuile PS3) : fais un `git pull` dessus, pas de clone neuf.

## Lis d'abord, dans cet ordre

1. `android/tools/quest3/BUILD-QUEST3.md` : outils, versions, variante, et la section « 2 bis » pour Windows (compilateur hôte LLVM-MinGW, `-DLLVM_HOST_TRIPLE=aarch64-none-linux-android33`, libadrenotools cloné en `--recursive` pour `lib/linkernsbypass`).
2. `android/tools/quest3/MULTIJOUEUR-QUEST3.md` : pistolets (GunCon 3, manettes Touch, WonderFitter), remote play joueur 2, et la section 6 sur le netplay de Tremu.
3. `android/tools/quest3/SPIKE-QUEST3.md` et `MEMOIRE-QUEST3.md` : pilotes Vulkan, stéréo, mémoire.
4. Le README d'ARMSX3, section Headsets.

## Ce qui a été fait côté ARMSX3 (jamais compilé, C++ vérifié en syntaxe seulement)

- Présentation OpenXR : `android/src/xr/xr_session.{h,cpp}`, hooks dans le core (`rpcs3/Emu/RSX/VK/vkutils/xr_hooks.h`, `instance.cpp`, `device.cpp`), écran virtuel en quad layer, pompe de frames découplée du flip RSX, 60 Hz demandé, verrou de soumission global autour des appels du runtime. Option CMake `ARMSX3_XR`.
- Stéréo par œil : `stereo_render_mode_options::side_by_side_full`, swapchain XR deux fois plus large quand `rsx::avconf::stereo_enabled`.
- Manettes Touch par actions OpenXR (profil Oculus Touch), haptique.
- Application : `com.armsx2.xr.XrEmulationActivity` (activité immersive, catégorie VR), `XrPresentation.kt`, préférences `xr.virtualScreen`, `xr.lightgun`, `xr.gunHand` ; réglages dans `RendererBackendSection.kt` ; retour au lanceur après un jeu lancé de l'extérieur (`MainActivityRuntime.returnToLauncherAfterExternalGame`).
- Pistolets : pad 1 en GunCon 3 (`_armsx3_setPadLightgun`), souris du core pilotée par l'app (`android_mouse_handler.h`, `_armsx3_mouseAim`), visée par le rayon « aim » de la manette, main automatique ou fixe (`_armsx3_xrSetGunHand`), calibration menu + gâchette au centre de l'écran (décalages dans `xr_lightgun_offsets.txt` du dossier de config), gâchette externe (`_armsx3_xrGunTrigger`).
- WonderFitter : `com.armsx2.xr.WonderFitterGun` (BLE, service FFF0, notifications FFF1, même protocole que Tremu). Tir → Croix, chargeur retiré → L1 maintenu, culasse → L1 150 ms. Permissions `BLUETOOTH_SCAN` et `BLUETOOTH_CONNECT` demandées par l'activité immersive.
- Remote play : paquet AIM (`RemotePlayProtocol.kt`, `PACKET_AIM = 8`), visée du viewer (`RemotePlayViewerActivity.kt`), application côté hôte sur le port 1 (`RemotePlayHost.kt`).
- Mémoire : gouverneur de pression (`_armsx3_memoryPressure`, `MemoryWatch.kt`), application de mesure `android/tools/quest3/memprobe`.
- Fusion des correctifs amont 1.6.2 (renommages `_armsx3_*`, `com.armsx3.core.ARMSX3`, cible CMake `armsx3-core`).

## Ce qui a été fait côté Tremu

- Netplay protocole 4 : état de partie compressé (zlib, morceaux de 1400 octets), CRC de RAM par table et échantillonné. Les deux casques doivent avoir le nouvel APK.
- Entrée : la confirmation (splash, carrousel, menu) fait un front par source (A droit, gâchette droite, tir WonderFitter, gâchette d'un pistolet Bluetooth) au lieu d'un front sur leur OU : une source bloquée ne masque plus les autres. Un axe de manette externe ne compte comme pressé qu'après avoir été vu au repos ; tout est relâché au retour au premier plan. Le Java journalise le premier tir puis un sur cinquante, et avertit si `nativeShot` ne résout pas.

## Ordre de vérification sur le matériel

Pour chaque point : résultat, extrait de logcat pertinent, correction faite s'il y en a une. Tags utiles : `XrEmulationActivity`, `XrPresentation`, `WonderFitterGun`, `RemotePlay`, le canal `armsx3_android` et le journal « XR » du core (fichier de log RPCS3), `Tremu-net`, `Tremu-gun`.

1. **Compilation** d'ARMSX3 (variante a13) et de Tremu. Les erreurs Kotlin/JNI les plus probables : signatures des nouvelles `external fun` dans `ARMSX3.kt` contre `native-lib.cpp`, imports manquants, API Bluetooth GATT selon le `compileSdk`.
2. **Régression 2D** : `xr.virtualScreen` désactivé, un jeu sur le panneau Horizon, comme avant.
3. **Écran virtuel** : session OpenXR créée avec le pilote Qualcomm (puis avec Turnip si disponible), quad visible, 60 Hz obtenu (`XR_FB_display_refresh_rate`), saccades mesurées avec la surcouche de performance. Comparer avec et sans la pompe découplée si ça saccade.
4. **Stéréo 3D** : réglage `ps3.stereo3d` avec un titre à sortie 3D native ; une image par œil, pas de décalage.
5. **Manettes Touch** : correspondance des boutons et sticks dans un jeu à pad, haptique.
6. **Retour au lanceur** : lancer un jeu depuis Tremu, le quitter, revenir sur Tremu.
7. **Pistolet avec les Touch** : `xr.lightgun` activé, Time Crisis 4. Précision de la visée, changement de main par la gâchette, réglage « Main du pistolet », calibration (menu maintenu + gâchette au centre), persistance de `xr_lightgun_offsets.txt`.
8. **WonderFitter** : demande de permissions, connexion (`Gun ... ready`), tir, chargeur, culasse ; passage du pistolet Tremu → ARMSX3 → Tremu.
9. **Remote play joueur 2** : second casque en viewer, visée envoyée (paquets AIM), survol reçu par le viewer, tir du joueur 2 dans Time Crisis 4 à deux GunCon 3.
10. **Mémoire** : lancer memprobe, consigner les seuils ; vérifier que `MemoryWatch` déclenche la pression avant le kill.
11. **Tremu, pistolet dans le carrousel** : WonderFitter connecté, le tir et le bouton A lancent un jeu depuis le carrousel et valident dans le menu ; en jeu, pas de tir continu. Dans le logcat (`Tremu-gun`) : `Gun ready`, puis `shot 1` au premier tir ; si un avertissement `shots do not reach the native side` apparaît, les noms JNI ne correspondent plus à la classe Java. La ligne « dernier signal » du menu dit quel contrôle externe arrive (un axe ou une touche bloqués y seraient visibles).
12. **Tremu netplay** : d'abord Terminator 2 (régression), puis un jeu PS2 et un Dreamcast à deux casques : temps d'arrivée de l'invité (`state ... packed to ... in ... ms`), absence de saccade à la seconde, nombre de désynchronisations sur dix minutes.

## À la fin

Mets à jour la section 5 « Ce qui est à vérifier sur le matériel » de `MULTIJOUEUR-QUEST3.md` et les notes de `BUILD-QUEST3.md` avec ce qui a été observé, pousse sur `master` (ARMSX3) et `main` (Tremu), et donne un rapport par point avec le verdict : fonctionne, corrigé, ou bloqué avec la cause.
