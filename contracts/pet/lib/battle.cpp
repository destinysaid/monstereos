using namespace types;
using namespace utils;

void pet::battlecreate(name host, battle_mode mode) {
  require_auth(host);

  battles tb_battles(_self, _self);
  auto itr_battle = tb_battles.find(host);
  eosio_assert(itr_battle == tb_battles.end(), "you already host a battle");
  eosio_assert(mode == V1 || mode == V2 || mode == V3, "invalid battle mode");

  tb_battles.emplace(_self, [&](auto& r) {
    r.host = host;
    r.mode = mode;
  });
}

void pet::battlejoin(name host, name player, checksum256 secret) {

  require_auth(player);

  battles tb_battles(_self, _self);
  auto itr_battle = tb_battles.find(host);
  eosio_assert(itr_battle != tb_battles.end(), "battle not found for current host");
  st_battle battle = *itr_battle;

  eosio_assert(!battle.player_exists(player), "player is already in this battle");
  eosio_assert(battle.commits.size() < 2, "battle is already full of players");

  battle.add_player(player, secret);

  tb_battles.modify(itr_battle, _self, [&](auto& r) {
    r.commits = battle.commits;
  });

}

void pet::battleleave(name host, name player) {

  require_auth(player);

  battles tb_battles(_self, _self);
  auto itr_battle = tb_battles.find(host);
  eosio_assert(itr_battle != tb_battles.end(), "battle not found for current host");
  st_battle battle = *itr_battle;

  eosio_assert(battle.started_at == 0, "battle already started");

  eosio_assert(battle.player_exists(player), "player not in this battle");

  if (player == host) {
    tb_battles.erase( itr_battle );
  } else {
    battle.remove_player(player);
    tb_battles.modify(itr_battle, 0, [&](auto& r) {
      r.commits = battle.commits;
    });
  }

}

void pet::battlestart(name host, name player, checksum256 source) {

  require_auth(player);

  battles tb_battles(_self, _self);
  auto itr_battle = tb_battles.find(host);
  eosio_assert(itr_battle != tb_battles.end(), "battle not found for current host");
  st_battle battle = *itr_battle;

  eosio_assert(battle.started_at == 0, "battle already started");
  eosio_assert(battle.commits.size() == 2, "battle has not enough players");

  // validates and summarize reveals
  bool valid_reveal = false;
  vector<st_commit> reveals{};
  for(auto& commit : battle.commits) {
    if (commit.player == player) {
      eosio_assert(is_zero(commit.reveal), "commit was already revealed");

      assert_sha256( (char *)&source, sizeof(source),
        (const checksum256 *)&commit.commitment );
      commit.reveal = source;
      reveals.emplace_back(commit);
      valid_reveal = true;
    } else if (!is_zero(commit.reveal)) {
      reveals.emplace_back(commit);
    }
  }
  eosio_assert(valid_reveal, "invalid reveal");

  // everybody commited, randomize all turns by random commits
  if (reveals.size() == battle.commits.size()) {
    std::sort (reveals.begin(), reveals.end(),
      [](const st_commit &a, const st_commit &b) -> bool {
        checksum256 result;
        sha256( (char *)&a, sizeof(b)*2, &result);
        return result.hash[1] < result.hash[0];
      });
    battle.started_at = now();
    battle.commits = reveals;
  }

  tb_battles.modify(itr_battle, 0, [&](auto& r) {
    r.started_at = battle.started_at;
    r.commits = battle.commits;
  });
}

void pet::battleselpet(name host, name player, uuid pet_id) {
  require_auth(player);

  battles tb_battles(_self, _self);
  auto itr_battle = tb_battles.find(host);
  eosio_assert(itr_battle != tb_battles.end(), "battle not found for current host");
  st_battle battle = *itr_battle;

  eosio_assert((battle.mode == V1 && battle.pets_stats.size() < 2) ||
    (battle.mode == V2 && battle.pets_stats.size() < 4) ||
    (battle.mode == V3 && battle.pets_stats.size() < 6),
    "all pets were selected already");

  battle.check_turn_and_rotate(player);

  auto itr_pet = pets.find(pet_id);
  eosio_assert(itr_pet != pets.end(), "E404|Invalid pet");
  st_pets pet = *itr_pet;

  // only owners can make pets sleep
  require_auth(pet.owner);

  battle.add_pet(pet_id, pet.type, player);

  tb_battles.modify(itr_battle, 0, [&](auto& r) {
    r.pets_stats = battle.pets_stats;
    r.commits = battle.commits;
  });

}

void pet::battleattack(name         host,
                       name         player,
                       uuid         pet_id,
                       uuid         pet_enemy_id,
                       element_type element) {

  require_auth(player);

  battles tb_battles(_self, _self);
  auto itr_battle = tb_battles.find(host);
  eosio_assert(itr_battle != tb_battles.end(), "battle not found for current host");
  st_battle battle = *itr_battle;

  battle.check_turn_and_rotate(player);

  uint8_t pet_type{0};
  uint8_t pet_enemy_type{0};
  bool valid_pet = false;
  for (const auto& pet_stat : battle.pets_stats) {
    if (pet_stat.pet_id == pet_id && pet_stat.player == player) {
      pet_type = pet_stat.pet_type;
      valid_pet = true;
    } else if (pet_stat.pet_id == pet_id && pet_stat.player != player) {
      eosio_assert(false, "you cannot control this monster");
    } else if (pet_stat.pet_id == pet_enemy_id) {
      pet_enemy_type = pet_stat.pet_type;
    }
  }

  eosio_assert(valid_pet, "invalid attack");

  st_pet_config pc = _get_pet_config();
  const auto& pet_types = pc.pet_types[pet_type];

  bool valid_element = false;
  for (const auto& pet_element : pet_types.elements) {
    if (pet_element == element) {
      valid_element = true;
      break;
    }
  }

  eosio_assert(valid_element, "invalid attack element");

  // cross ratio elements to enemy pet elements
  uint8_t ratio{8}; // default ratio
  const auto& element_types = pc.element_types[element];
  const auto& pet_enemy_types = pc.pet_types[pet_enemy_type];
  for (const auto& pet_element : pet_enemy_types.elements) {
    for (const auto& type_ratio : element_types.ratios) {

      // get the biggest ratio
      if (type_ratio.type == pet_element && type_ratio.ratio > ratio) {
        ratio = type_ratio.ratio;
        break;
      }
    }
  }

  // add damage and dice randomization based on commits here
  for (auto& pet_stat : battle.pets_stats) {
    if (pet_stat.pet_id == pet_enemy_id) {
      pet_stat.hp = pet_stat.hp - ratio;
      break;
    }
  }

  tb_battles.modify(itr_battle, 0, [&](auto& r) {
    r.pets_stats = battle.pets_stats;
    r.commits = battle.commits;
  });

}
