    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    context->ctr = context->gpr[5];
    }
    {
    context->gpr[3] = context->gpr[3] + 0xFFFFFFFCu;
    }
    {
    context->gpr[4] = context->gpr[4] + 0xFFFFFFFCu;
    }
label_8000440C:
    {
    const std::uint32_t ea_8000440C = context->gpr[4] + 0x00000004u;
    context->gpr[6] = galaxy::guest_load_u32(memory, ea_8000440C, services, 0x8000440Cu);
    context->gpr[4] = ea_8000440C;
    }
    {
    const std::uint32_t ea_80004410 = context->gpr[3] + 0x00000004u;
    galaxy::guest_store_u32(memory, ea_80004410, context->gpr[6], services, 0x80004410u);
    context->gpr[3] = ea_80004410;
    }
    {
    if (--context->ctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; galaxy::branch_checkpoint_taken_budgeted(services, 0x80004414u, 0x8000440Cu, context, memory); } goto label_8000440C; }
    }
    {
    return;
    }
