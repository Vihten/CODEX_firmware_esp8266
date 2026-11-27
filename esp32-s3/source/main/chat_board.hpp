#pragma once
#include <cstdint>

struct ChatBoardMessage {
  char id[37];
  uint32_t sender;
  char name[49];
  char text[513];
};

bool chatBoardTake(ChatBoardMessage &message);
