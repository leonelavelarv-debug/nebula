    std::uint32_t gr3 = context->gpr[3];
    std::uint32_t gr4 = context->gpr[4];
    std::uint64_t fpr[32];
    std::uint64_t ps1[32];
    fpr[1] = context->fpr_bits[1];
    fpr[2] = context->fpr_bits[2];
    ps1[1] = context->ps1_bits[1];
    ps1[2] = context->ps1_bits[2];
    [[maybe_unused]] std::byte* const flat_guest_base = galaxy::fast::flat_base(memory);
    [[maybe_unused]] const std::byte* flat_guest_read_base = flat_guest_base;
    [[maybe_unused]] std::uint32_t checkpoint_budget__ = galaxy::kBranchCheckpointBudget;
    {
    fpr[1] = galaxy::fast::load_u64(flat_guest_read_base, memory, gr3, services, 0x80004600u);
    }
    {
    galaxy::fast::fctiwz_r(context, fpr, ps1, 2u, 1u, false, services, 0x80004604u);
    }
    {
    galaxy::fast::store<std::uint64_t>(flat_guest_base, memory, gr4, fpr[2], services, 0x80004608u);
    }
    {
    { context->fpr_bits[1] = fpr[1]; context->fpr_bits[2] = fpr[2]; context->ps1_bits[2] = ps1[2]; return; }
    }
