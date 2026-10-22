    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    context->gpr[5] = 0x00000000u;
    }
    {
    context->ctr = context->gpr[4];
    }
    {
    context->gpr[3] = context->gpr[3] + 0xFFFFFFFCu;
    }
label_8000400C:
    {
    const std::uint32_t ea_8000400C = context->gpr[3] + 0x00000004u;
    context->gpr[6] = galaxy::guest_load_u32(memory, ea_8000400C, services, 0x8000400Cu);
    context->gpr[3] = ea_8000400C;
    }
    {
    context->gpr[5] = context->gpr[5] + context->gpr[6];
    }
    {
    if (--context->ctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; galaxy::branch_checkpoint_taken_budgeted(services, 0x80004014u, 0x8000400Cu, context, memory); } goto label_8000400C; }
    }
    {
    context->gpr[3] = context->gpr[5];
    }
    {
    return;
    }
