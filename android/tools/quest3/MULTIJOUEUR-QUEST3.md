# Multijoueur à deux casques, et Time Crisis 4 à deux

Objectif : deux Quest 3, un jeu, chaque joueur voit le jeu dans son casque et vise avec sa
propre manette. Cas de référence : Time Crisis 4 (PS3, GunCon 3, deux joueurs sur le même
écran).

## 1. Architecture retenue : un hôte émule, l'autre casque reçoit

RPCS3 n'a pas de netplay déterministe et ne pourra pas en avoir à court terme : faire tourner
deux instances synchronisées est exclu. La seule architecture viable est celle qu'ARMSX3
possède déjà, le **remote play** : l'hôte émule, encode la vidéo (WebRTC, H.264 matériel),
l'audio (Opus) et reçoit le pad du joueur 2 par un canal de données non fiable, sur le même
Wi-Fi (découverte DNS-SD) ou par Internet (UPnP). Le viewer est une activité 2D ; dans un
casque, Horizon OS l'affiche comme un panneau flottant.

Ce que le dépôt n'avait pas pour un jeu de tir, et qui est livré ici :

| Besoin | Avant | Maintenant |
|--------|-------|------------|
| Un pistolet PS3 | aucun : `Lightgun.kt` appelait des stubs PCSX2 (`Unsupported.note`) | le pad devient un **GunCon 3** (identité USB Namco 0x0B9A/0x0800), celui que Time Crisis 4 attend |
| Une source de visée | aucun gestionnaire de souris Android ; le GunCon 3 du core lit la souris | `android_mouse_handler.h` : une souris par port de pad, pilotée par l'app, réglage « Mouse » forcé à `raw` pour que chaque pistolet lise la sienne |
| Viser dans le casque | rien | le rayon de visée (`aim pose`) de la manette droite coupe l'écran virtuel ; le point d'impact devient la souris 0, la gâchette devient Croix (la gâchette du GunCon 3) |
| Viser depuis le casque du joueur 2 | rien | le pointeur de sa manette sur le panneau du viewer (survol et appui) est envoyé à l'hôte en paquet « aim » ; l'hôte le met dans la souris 1 et la gâchette dans Croix du pad 2 |
| Viser sur téléphone | stubs | la couche tactile existante, rebranchée sur la souris du core |

## 2. Comment jouer à Time Crisis 4 à deux

Hôte (casque 1) : Paramètres > Rendu > « Écran virtuel » et « Pistolet avec la manette
droite » activés, puis lancer le jeu. Ouvrir une session remote play (même Wi-Fi). Le pad 1
est un GunCon 3 ; dès qu'un ami rejoint, le pad 2 en devient un aussi.

Invité (casque 2) : rejoindre la session dans ARMSX3 (viewer). Pointer la manette vers le
panneau vidéo : le survol vise, la gâchette tire.

Dans Time Crisis 4, le pédalier de couverture est sur les boutons A/B du GunCon 3, c'est-à-dire
L1/R1 du pad : les grips des manettes Touch chez l'hôte, et les boutons du pad virtuel chez
l'invité.

## 3. Ce que la latence impose

Le joueur 2 vise sur une image qui a traversé l'encodeur, le Wi-Fi et le décodeur : comptez
40 à 80 ms sur un bon Wi-Fi 5 GHz, davantage sur un réseau chargé. Pour un rail shooter c'est
jouable mais ce n'est pas la précision de l'hôte. Trois leviers, par ordre d'efficacité :

1. **Le Wi-Fi** : routeur à moins de dix mètres, bande 5 GHz dédiée, pas d'autre trafic vidéo.
   La stat `HostStats` du remote play (RTT) dit où l'on en est.
2. **La résolution du flux** : 720p suffit, c'est la sortie native de la PS3.
3. **Un viewer immersif** (phase 2) : afficher le flux directement en couche OpenXR dans le
   casque 2 au lieu du panneau Horizon économise la composition du panneau et donne un
   écran de la même taille que chez l'hôte. Ce n'est pas un gain de latence réseau.

## 4. Phase 2 : le viewer immersif

Aujourd'hui le viewer est un panneau 2D. Le passage en OpenXR demande un rendu propre au
viewer (pas de core) : flux MediaCodec vers une SurfaceTexture, dessinée sur la XrSwapchain
par GLES (XR_KHR_opengl_es_enable) sur un quad identique à celui de l'hôte, et la visée par
le même rayon que l'hôte. Deux à trois jours de travail, à lancer une fois que la phase 1 est
vérifiée sur deux casques : le pipeline hôte, le GunCon 3 et l'injection de la visée ne
changent pas d'une phase à l'autre.

## 5. Ce qui est à vérifier sur le matériel

- Time Crisis 4 démarre-t-il avec un GunCon 3 sur le pad 1 et le reconnaît-il (journal :
  « Adding emulated GunCon3 (controller 0) ») ? Idem pour le pad 2 quand l'ami rejoint.
- La visée : le point d'impact du rayon correspond-il au curseur du jeu ? Le calcul suppose
  que l'écran virtuel est à `distance_m` devant l'origine de l'espace LOCAL, ce qui est la
  géométrie du quad ; un recentrage du casque en cours de jeu déplace les deux ensemble.
- Chez l'invité : Horizon OS livre-t-il bien les survols du pointeur au `SurfaceViewRenderer`
  (`ACTION_HOVER_MOVE`) ? Sinon, seule la visée gâchette enfoncée fonctionne, et il faudra
  lire le pointeur autrement (phase 2 le règle de toute façon).
- Le conseil communautaire d'une limite de 30 fps pour la précision de tir de Time Crisis 4
  sous RPCS3 est à tester sur le casque.

Sources : [support deux GunCon 3 sur PS3](https://consumerist.com/2007/12/05/namco-bandai-wont-sell-accessory-separately), [forum arcade, limite 30 fps](https://forum.arcadecontrols.com/index.php/topic,163489.180/wap2.html).
