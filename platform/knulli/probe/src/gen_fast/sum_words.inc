    std::uint32_t gr3 = context->gpr[3];
    std::uint32_t gr4 = context->gpr[4];
    std::uint32_t gr5 = context->gpr[5];
    std::uint32_t gr6 = context->gpr[6];
    std::uint32_t gctr = context->ctr;
    [[maybe_unused]] std::byte* const flat_guest_base = galaxy::fast::flat_base(memory);
    [[maybe_unused]] const std::byte* flat_guest_read_base = flat_guest_base;
    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    gr5 = 0x00000000u;
    }
    {
    gctr = gr4;
    }
    {
    gr3 = gr3 + 0xFFFFFFFCu;
    }
label_8000400C:
    {
    const std::uint32_t ea_8000400C = gr3 + 0x00000004u;
    gr6 = galaxy::guest_load_flat_or_checked_u32(flat_guest_read_base, memory, ea_8000400C, services, 0x8000400Cu);
    gr3 = ea_8000400C;
    }
    {
    gr5 = gr5 + gr6;
    }
    {
    if (--gctr != 0u) { if (--checkpoint_budget__ == 0u) [[unlikely]] { checkpoint_budget__ = galaxy::kBranchCheckpointBudget; if (const std::uint64_t counted__ = galaxy::branch_checkpoint_due_budgeted(services); counted__ != 0u) [[unlikely]] { GALAXY_RETURN_IF_TRANSFER_PENDING(services); context->gpr[3] = gr3; context->gpr[5] = gr5; context->gpr[6] = gr6; context->ctr = gctr; galaxy::branch_checkpoint_taken_counted(services, 0x80004014u, 0x8000400Cu, context, memory, counted__); gr3 = context->gpr[3]; gr4 = context->gpr[4]; gr5 = context->gpr[5]; gr6 = context->gpr[6]; gctr = context->ctr; GALAXY_RETURN_IF_TRANSFER_PENDING(services); } } goto label_8000400C; }
    }
    {
    gr3 = gr5;
    }
    {
    { context->gpr[3] = gr3; context->gpr[5] = gr5; context->gpr[6] = gr6; context->ctr = gctr; return; }
    }
