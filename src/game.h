#pragma once

namespace game {

// Hooks FViewport::Draw so every game frame is rendered once per eye. The rest
// of the game-side hooks are installed lazily from the game thread.
void InstallDrawHook();

} // namespace game
