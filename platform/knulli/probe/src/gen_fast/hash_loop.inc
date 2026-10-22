    std::uint32_t gr3 = context->gpr[3];
    std::uint32_t gr4 = context->gpr[4];
    std::uint32_t gr5 = context->gpr[5];
    std::uint32_t gr7 = context->gpr[7];
    std::uint32_t gctr = context->ctr;
    [[maybe_unused]] std::byte* const flat_guest_base = galaxy::fast::flat_base(memory);
    [[maybe_unused]] const std::byte* flat_guest_read_base = flat_guest_base;
    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    gr7 = 0x9E370000u;
    }
    {
    gr7 = gr7 | 0x000079B9u;
    }
    {
    gctr = gr4;
    }
label_8000420C:
    {
    gr5 = std::rotl(gr3, 13) & 0xFFFFE000u;
    }
    {
    gr3 = gr3 ^ gr5;
    }
    {
    gr5 = std::rotl(gr3, 15) & 0x00007FFFu;
    }
    {
    gr3 = gr3 ^ gr5;
    }
    {
    gr5 = std::rotl(gr3, 5) & 0xFFFFFFE0u;
    }
    {
    gr3 = gr3 ^ gr5;
    }
    {
    const std::int64_t result = static_cast<std::int64_t>(static_cast<std::int32_t>(gr3)) * static_cast<std::int32_t>(gr7);
    gr3 = static_cast<std::uint32_t>(result);
    }
    {
    if (--gctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; if (const std::uint64_t counted__ = galaxy::branch_checkpoint_due_budgeted(services); counted__ != 0u) [[unlikely]] { GALAXY_RETURN_IF_TRANSFER_PENDING(services); context->gpr[3] = gr3; context->gpr[5] = gr5; context->gpr[7] = gr7; context->ctr = gctr; galaxy::branch_checkpoint_taken_counted(services, 0x80004228u, 0x8000420Cu, context, memory, counted__); gr3 = context->gpr[3]; gr4 = context->gpr[4]; gr5 = context->gpr[5]; gr7 = context->gpr[7]; gctr = context->ctr; GALAXY_RETURN_IF_TRANSFER_PENDING(services); } } goto label_8000420C; }
    }
    {
    { context->gpr[3] = gr3; context->gpr[5] = gr5; context->gpr[7] = gr7; context->ctr = gctr; return; }
    }
