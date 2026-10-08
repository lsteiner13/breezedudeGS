#pragma once

// Muss regelmäßig aus loop() aufgerufen werden.
// Prüft beim ersten WLAN-Connect sofort und danach alle 10 Minuten,
// ob Internet vorhanden ist. Falls nicht, wird der KitzSki-Login versucht.
void kitzskiPortalTick();

// Liefert true, wenn der letzte Internet-Test erfolgreich war.
bool kitzskiInternetReady();