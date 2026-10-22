    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    context->gpr[7] = 0x9E370000u;
    }
    {
    context->gpr[7] = context->gpr[7] | 0x000079B9u;
    }
    {
    context->ctr = context->gpr[4];
    }
label_8000420C:
    {
    context->gpr[5] = std::rotl(context->gpr[3], 13) & 0xFFFFE000u;
    }
    {
    context->gpr[3] = context->gpr[3] ^ context->gpr[5];
    }
    {
    context->gpr[5] = std::rotl(context->gpr[3], 15) & 0x00007FFFu;
    }
    {
    context->gpr[3] = context->gpr[3] ^ context->gpr[5];
    }
    {
    context->gpr[5] = std::rotl(context->gpr[3], 5) & 0xFFFFFFE0u;
    }
    {
    context->gpr[3] = context->gpr[3] ^ context->gpr[5];
    }
    {
    const std::int64_t result = static_cast<std::int64_t>(static_cast<std::int32_t>(context->gpr[3])) * static_cast<std::int32_t>(context->gpr[7]);
    context->gpr[3] = static_cast<std::uint32_t>(result);
    }
    {
    if (--context->ctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; galaxy::branch_checkpoint_taken_budgeted(services, 0x80004228u, 0x8000420Cu, context, memory); } goto label_8000420C; }
    }
    {
    return;
    }
