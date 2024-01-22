

static void afk_recv_handle_init(struct apple_dcp_afkep *ep, u32 channel,
				 u8 *payload, size_t payload_size)
{
	char name[32];
	s64 epic_unit = -1;
	u32 ch_idx;
	const char *service_name = name;
	const char *epic_name = NULL, *epic_class = NULL;
	const struct apple_epic_service_ops *ops;
	struct dcp_parse_ctx ctx;
	u8 *props = payload + sizeof(name);
	size_t props_size = payload_size - sizeof(name);

	WARN_ON(afk_epic_find_service(ep, channel));

	if (payload_size < sizeof(name)) {
		dev_err(ep->dev, "AFK[ep:%02x]: payload too small: %lx\n",
			ep->endpoint, payload_size);
		return;
	}

	if (ep->num_channels >= AFK_MAX_CHANNEL) {
		dev_err(ep->dev, "AFK[ep:%02x]: too many enabled services!\n",
			ep->endpoint);
		return;
	}

	strlcpy(name, payload, sizeof(name));

	/*
	 * in DCP firmware 13.2 DCP reports interface-name as name which starts
	 * with "dispext%d" using -1 s ID for "dcp". In the 12.3 firmware
	 * EPICProviderClass was used. If the init call has props parse them and
	 * use EPICProviderClass to match the service.
	 */
	if (props_size > 36) {
		int ret = parse(props, props_size, &ctx);
		if (ret) {
			dev_err(ep->dev,
				"AFK[ep:%02x]: Failed to parse service init props for %s\n",
				ep->endpoint, name);
			return;
		}
		ret = parse_epic_service_init(&ctx, &epic_name, &epic_class, &epic_unit);
		if (ret) {
			dev_err(ep->dev,
				"AFK[ep:%02x]: failed to extract init props: %d\n",
				ep->endpoint, ret);
			return;
		}
		service_name = epic_class;
	} else {
            service_name = name;
        }

	ops = afk_match_service(ep, service_name);
	if (!ops) {
		dev_err(ep->dev,
			"AFK[ep:%02x]: unable to match service %s on channel %d\n",
			ep->endpoint, service_name, channel);
		goto free;
	}

	ch_idx = ep->num_channels++;
	spin_lock_init(&ep->services[ch_idx].lock);
	ep->services[ch_idx].enabled = true;
	ep->services[ch_idx].ops = ops;
	ep->services[ch_idx].ep = ep;
	ep->services[ch_idx].channel = channel;
	ep->services[ch_idx].cmd_tag = 0;
	ops->init(&ep->services[ch_idx], epic_name, epic_class, epic_unit);
	dev_info(ep->dev, "AFK[ep:%02x]: new service %s on channel %d\n",
		 ep->endpoint, service_name, channel);
free:
	kfree(epic_name);
	kfree(epic_class);
}
