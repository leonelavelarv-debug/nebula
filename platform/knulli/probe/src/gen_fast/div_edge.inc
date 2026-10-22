    std::uint32_t gr3 = context->gpr[3];
    std::uint32_t gr4 = context->gpr[4];
    [[maybe_unused]] std::byte* const flat_guest_base = galaxy::fast::flat_base(memory);
    [[maybe_unused]] const std::byte* flat_guest_read_base = flat_guest_base;
    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    gr3 = galaxy::divide_signed_word(gr3, gr4, services, 0x80004300u);
    }
    {
    { context->gpr[3] = gr3; return; }
    }
